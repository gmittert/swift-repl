#include "REPL.h"
#include "Logging.h"
#include "TransformAST.h"
#include "TransformIR.h"
#include "Config.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>

#include <swift/AST/ASTMangler.h>
#include <swift/SILOptimizer/PassManager/Passes.h>

void ConfigureFunctionLinkage(swift::SourceFile &src_file, std::unique_ptr<swift::SILModule> &sil_module)
{
    SetCurrentLoggingArea(LoggingArea::SIL);

    for(swift::Decl *decl : src_file.Decls)
    {
        assert(decl);
        if(auto fn = llvm::dyn_cast<swift::FuncDecl>(decl))
        {
            swift::SILDeclRef sil_decl(fn);

            std::string name_original = fn->getName().str();
            std::string name_mangled = sil_decl.mangle();

            sil_module->lookUpFunction(sil_decl)->setLinkage(swift::SILLinkage::Public);
            Log(std::string("Set function ") + name_original + " (" + name_mangled + ") to public");
        }
    }
    sil_module->lookUpFunction("main")->setLinkage(swift::SILLinkage::Private);
}

void REPL::RemoveRedeclarationsFromJIT(std::unique_ptr<llvm::Module> &llvm_module)
{
    SetCurrentLoggingArea(LoggingArea::SIL);

    for(const llvm::Function &fn : llvm_module->functions())
    {
        if(fn.isDeclaration() || !fn.hasExternalLinkage())
            continue;
        std::string name = fn.getName().str();

        if(llvm::Error err = m_jit->RemoveSymbol(name))
        {
            SetCurrentLoggingArea(LoggingArea::JIT);
            llvm::handleAllErrors(std::move(err),
                                  [&](const llvm::orc::SymbolsCouldNotBeRemoved &)
                                  {
                                      Log((llvm::Twine("Could not remove symbol") + name).str(), LoggingPriority::Error);
                                  },
                                  [](const llvm::orc::SymbolsNotFound &) { /* pass */ });
        }

        if(m_fn_ptr_map.find(name) == m_fn_ptr_map.end())
            m_fn_ptr_map[name] = "";
    }
}

llvm::Expected<std::unique_ptr<REPL>> REPL::Create(
    bool is_playground,
    std::string default_module_cache_path)
{
    std::unique_ptr<REPL> result(new REPL(is_playground, default_module_cache_path));
    auto jit = JIT::Create();
    SetCurrentLoggingArea(LoggingArea::JIT);
    if(!jit)
    {
        Log("Failed to initialize JIT", LoggingPriority::Error);
        return jit.takeError();
    }
    result->m_jit = std::move(*jit);
    return std::unique_ptr<REPL>(std::move(result));
}

REPL::REPL(bool is_playground, std::string default_module_cache_path)
    : m_is_playground(is_playground),
      m_default_module_cache_path(default_module_cache_path),
      m_curr_input_number(1),
      m_diagnostic_engine(m_src_mgr),
      m_ast_ctx(swift::ASTContext::get(m_lang_opts, m_spath_opts, m_src_mgr,
                                       m_diagnostic_engine))
{
    static bool s_run_guard = false;
    if(!s_run_guard)
    {
        INITIALIZE_LLVM();
        s_run_guard = true;
    }

    m_diagnostic_engine.setShowDiagnosticsAfterFatalError();
    m_diagnostic_engine.addConsumer(m_diagnostic_consumer);

    SetupLangOpts();
    SetupSearchPathOpts();
    SetupSILOpts();
    SetupIROpts();
    SetupImporters();
    swift::registerTypeCheckerRequestFunctions(m_ast_ctx->evaluator);
}

std::string REPL::GetLine()
{
    std::cout << "\n";
    std::string result = "";
    do
    {
        std::cout << m_curr_input_number << "> ";
        std::getline(std::cin, result);
    } while(result.empty());
    return result;
}

void REPL::AddModuleSearchPath(std::string path)
{
    m_ast_ctx->addSearchPath(path, false, false);
}

void REPL::AddFrameworkSearchPath(std::string path)
{
    m_ast_ctx->addSearchPath(path, true, false);
}

void REPL::AddLoadSearchPath(std::string path)
{
    m_jit->AddSearchPath(path);
}

bool REPL::IsExitString(const std::string &line)
{
    return line == "e" || line == "exit";
}

// NOTE(sasha): We don't use the normal logging system here because the
//              DiagnosticEngine will have shown the error.
// TODO(sasha): Make this not print to stdout
#define CHECK_ERROR() if(m_diagnostic_engine.hadAnyError()) { return true; }
#define PRINT_INVALID_REDECLARATION(name) { std::cout << "Invalid redeclaration of " << name << "\n"; return true; }

bool REPL::ExecuteSwift(std::string line)
{
    m_curr_input_number++;
    m_diagnostic_engine.resetHadAnyError();

    if(IsExitString(line))
        return false;

    swift::Mangle::ASTMangler mangler;

    ReplInput input = AddToSrcMgr(line);
    auto repl_module_id = m_ast_ctx->getIdentifier("__REPL__");
    auto *repl_module = swift::ModuleDecl::create(repl_module_id, *m_ast_ctx);
    CHECK_ERROR();
    constexpr auto implicit_import_kind =
        swift::SourceFile::ImplicitModuleImportKind::Stdlib;
    m_invocation.getFrontendOptions().ModuleName = input.module_name.c_str();
    m_invocation.getIRGenOptions().ModuleName = input.module_name.c_str();

    swift::SourceFile *tmp_src_file = new (*m_ast_ctx) swift::SourceFile(
        *repl_module, swift::SourceFileKind::Main, input.buffer_id,
        implicit_import_kind);
    if(!tmp_src_file)
    {
        Log("Unable to create SourceFile!", LoggingPriority::Error);
        return false;
    }
    repl_module->addFile(*tmp_src_file);

    CHECK_ERROR();
    swift::PersistentParserState persistent_state(*m_ast_ctx);

    bool done = false;
    do
    {
        swift::parseIntoSourceFile(*tmp_src_file,
                                   input.buffer_id,
                                   &done,
                                   nullptr /* SILParserState */,
                                   &persistent_state,
                                   false /* DelayBodyParsing */);
        CHECK_ERROR();
    } while(!done);
    SetCurrentLoggingArea(LoggingArea::AST);
    if(ShouldLog(LoggingPriority::Info))
    {
        Log("=========AST Before Modifications==========");
        tmp_src_file->dump();
    }
    AddImportNodes(*tmp_src_file, m_imports);

    swift::performNameBinding(*tmp_src_file);
    CHECK_ERROR();
    swift::TopLevelContext top_level_context;
    swift::OptionSet<swift::TypeCheckingFlags> type_check_opts;
    swift::performTypeChecking(*tmp_src_file, top_level_context, type_check_opts);
    
    ModifyAST(*tmp_src_file);
    
    CHECK_ERROR();
    swift::typeCheckExternalDefinitions(*tmp_src_file);
    CHECK_ERROR();

    if(ShouldLog(LoggingPriority::Info))
    {
        Log("=========AST After Modification==========");
        tmp_src_file->dump();
    }
    LoadImportedModules(*tmp_src_file);
    repl_module->collectLinkLibraries([&](swift::LinkLibrary library)
                                      {
                                          m_jit->AddDylib(library.getName().str());
                                      });

    swift::FuncDecl *res_fn = nullptr;
    for(swift::Decl *decl : tmp_src_file->Decls)
    {
        if(!llvm::isa<swift::ValueDecl>(decl))
            continue;

        swift::ValueDecl *v_decl = llvm::dyn_cast<swift::ValueDecl>(decl);
        std::string unmangled_name = "";
        std::string name = "";
        // NOTE(sasha): Two functions can have the same unmangled name, but no other
        //              pair declaration types can have the same unmangled name
        //              (e.g. Function-Variable, Class-Variable, Function-Class are
        //               all not allowed. Only Function-Function is allowed).
        if(swift::FuncDecl *fn_decl = llvm::dyn_cast<swift::FuncDecl>(v_decl))
        {
            unmangled_name = fn_decl->getName().str();
            if(unmangled_name == input.module_name)
                res_fn = fn_decl;

            name = mangler.mangleEntity(v_decl, false);
            if(m_decl_map.find(unmangled_name) != m_decl_map.end())
            {
                assert(m_decl_map[unmangled_name]->Decls.size() == 1);
                if(!llvm::isa<swift::FuncDecl>(m_decl_map[unmangled_name]->Decls[0]))
                    PRINT_INVALID_REDECLARATION(unmangled_name);
            }
            // Don't allow redefinitions of any kind in playgrounds
            if(m_is_playground && m_decl_map.find(name) != m_decl_map.end())
                PRINT_INVALID_REDECLARATION(unmangled_name);
        }
        else
        {
            unmangled_name = v_decl->getBaseName().getIdentifier().str();
            name = unmangled_name;
            if(m_decl_map.find(name) != m_decl_map.end())
                PRINT_INVALID_REDECLARATION(name);
        }
        swift::Identifier new_module_id = m_ast_ctx->getIdentifier(name);
        swift::ModuleDecl *new_module = swift::ModuleDecl::create(new_module_id,
                                                                  *m_ast_ctx);
        swift::SourceFile *src_file;
        if(m_decl_map.find(name) == m_decl_map.end())
        {
            src_file = new (*m_ast_ctx) swift::SourceFile(
                *new_module, swift::SourceFileKind::Main, input.buffer_id,
                implicit_import_kind, false);

            swift::ImportDecl *new_module_import_decl = swift::ImportDecl::create(
                *m_ast_ctx, src_file, swift::SourceLoc(),
                swift::ImportKind::Module, swift::SourceLoc(),
                { { new_module_id, swift::SourceLoc() } });
            new_module_import_decl->setImplicit(true);
            m_imports.push_back(new_module_import_decl);
        }
        else
        {
            src_file = m_decl_map[name];
        }

        m_ast_ctx->LoadedModules[new_module_id] = new_module;
        m_decl_map[unmangled_name] = src_file;
        m_decl_map[name] = src_file;
        new_module->addFile(*src_file);
        src_file->Decls = { decl };
        src_file->ASTStage = swift::SourceFile::ASTStage_t::TypeChecked;

        if(ShouldLog(LoggingPriority::Info))
        {
            Log(std::string("=========AST for ") + name + "==========");
            src_file->dump();
        }

        if(!CompileSourceFileToIRAndAddToJIT(*src_file))
            return true;
    }

    SetCurrentLoggingArea(LoggingArea::JIT);
    if(res_fn)
    {
        std::string mangled_fn_name = mangler.mangleEntity(res_fn, false);

        using ReplFn = std::add_pointer<void()>::type;
        ReplFn result_fn = nullptr;
        if(auto symbol = m_jit->LookupSymbol(mangled_fn_name))
        {
            result_fn = reinterpret_cast<ReplFn>(symbol->getAddress());
            Log(std::string("Loaded function ") + mangled_fn_name);
            result_fn();
        }
        else
        {
            Log(std::string("Failed to load function ") + mangled_fn_name, LoggingPriority::Error);
            return false;
        }
    }
    return true;
}

llvm::Error REPL::UpdateFunctionPointers()
{
    for(const auto &name : m_fn_ptr_map)
    {
        auto fn = m_jit->LookupSymbol(name.first);
        if(fn)
        {
            auto ptr = m_jit->LookupSymbol(name.second);
            if(ptr)
                *reinterpret_cast<std::uintptr_t *>(ptr->getAddress()) =
                    static_cast<std::uintptr_t>(fn->getAddress());
            else
                return ptr.takeError();
        }
        else
        {
            return fn.takeError();
        }
    }
    return llvm::Error::success();
}


bool REPL::CompileSourceFileToIRAndAddToJIT(swift::SourceFile &src_file)
{
    swift::Lowering::TypeConverter type_converter(*src_file.getParentModule());
    std::unique_ptr<swift::SILModule> sil_module(
        swift::performSILGeneration(src_file,
                                    type_converter,
                                    m_invocation.getSILOptions()));
    CHECK_ERROR();
    ConfigureFunctionLinkage(src_file, sil_module);
    swift::runSILDiagnosticPasses(*sil_module);
    CHECK_ERROR();
    SetCurrentLoggingArea(LoggingArea::SIL);
    if(ShouldLog(LoggingPriority::Info))
    {
        Log("=========SIL==========");
        sil_module->dump();
    }
    CHECK_ERROR();
    std::unique_ptr<llvm::Module> llvm_module(swift::performIRGeneration(m_invocation.getIRGenOptions(),
                                                                         src_file,
                                                                         std::move(sil_module),
                                                                         "swift_repl_module",
                                                                         swift::PrimarySpecificPaths(),
                                                                         m_llvm_ctx));
    SetCurrentLoggingArea(LoggingArea::IR);
    if(ShouldLog(LoggingPriority::Info))
    {
        Log("Symbols in IR:");
        for(auto &g : llvm_module->global_values())
            std::cout << '\t' << g.getName().str() << '\n';

        std::string llvm_ir;
        llvm::raw_string_ostream str_stream(llvm_ir);
        str_stream << "=========LLVM IR==========\n";
        llvm_module->print(str_stream, nullptr);
        str_stream.flush();
        Log(llvm_ir);
    }

    RemoveRedeclarationsFromJIT(llvm_module);
    AddFunctionPointers(llvm_module, m_jit, m_llvm_ctx, m_fn_ptr_map);
    ReplaceFunctionCallsWithIndirectFunctionCalls(llvm_module,
                                                  m_llvm_ctx,
                                                  m_fn_ptr_map);

    SetCurrentLoggingArea(LoggingArea::IR);
    if(ShouldLog(LoggingPriority::Info))
    {
        std::string llvm_ir;
        llvm::raw_string_ostream str_stream(llvm_ir);
        str_stream << "=========New LLVM IR==========\n";
        llvm_module->print(str_stream, nullptr);
        str_stream.flush();
        Log(llvm_ir);
    }

    m_jit->AddModule(std::move(llvm_module));

    if(UpdateFunctionPointers())
    {
        Log("Unable to update function pointers", LoggingPriority::Error);
        return false;
    }
    return true;
}

void REPL::LoadImportedModules(swift::SourceFile &src_file)
{
    for(swift::Decl *decl : src_file.Decls)
    {
        if(auto *import_decl = llvm::dyn_cast<swift::ImportDecl>(decl))
            m_imports.push_back(import_decl);
    }
}

// ModifyAST performs four modifications on AST:
//    - Add global variable of same type as last expression.
//    - Modify last expression to be assignment to this global variable.
//    - Wrap existing AST into a function called  __repl_x where x is the REPL line number
//      (generated in AddToSrcMgr). We do this so that we don't have to remake the JIT object
//      every time we execute a new REPL line. Later, we lookup the function by name from the
//      JIT and call it and print out result.
//    - Make all declarations public (except classes which will be made open) so that our
//      our function actually gets generated. 
void REPL::ModifyAST(swift::SourceFile &src_file)
{
    CombineTopLevelDeclsAndMoveToBack(src_file);
    TransformFinalExpressionAndAddGlobal(src_file);
    WrapInFunction(src_file);
    MakeDeclarationsPublic(src_file);
}

REPL::ReplInput REPL::AddToSrcMgr(const std::string &line)
{
    ReplInput result;
    result.text = line;
    llvm::raw_string_ostream stream(result.module_name);
    stream << "__repl_" << m_curr_input_number;
    stream.flush();
    std::unique_ptr<llvm::MemoryBuffer> mb = llvm::MemoryBuffer::getMemBufferCopy(line, result.module_name);
    result.buffer_id = m_src_mgr.addNewSourceBuffer(std::move(mb));
    return result;
}

void REPL::SetupLangOpts()
{
    m_lang_opts.Target.setArch(llvm::Triple::ArchType::x86_64);
    m_lang_opts.Target.setOS(llvm::Triple::OSType::Win32);
    m_lang_opts.Target.setEnvironment(llvm::Triple::EnvironmentType::MSVC);
    m_lang_opts.Target.setObjectFormat(llvm::Triple::ObjectFormatType::COFF);
    m_lang_opts.EnableObjCInterop = false;
    m_lang_opts.EnableDollarIdentifiers = true;
    m_lang_opts.EnableAccessControl = true;
    m_lang_opts.EnableTargetOSChecking = false;
    m_lang_opts.Playground = true;
    m_lang_opts.EnableThrowWithoutTry = true;
}

void REPL::SetupSearchPathOpts()
{
    AddModuleSearchPath(SWIFT_BUILTIN_MODULE_PATH);
}

void REPL::SetupSILOpts()
{
    swift::SILOptions &sil_opts = m_invocation.getSILOptions();
    sil_opts.DisableSILPerfOptimizations = true;
    sil_opts.OptMode = swift::OptimizationMode::NoOptimization;
}

void REPL::SetupIROpts()
{
    swift::IRGenOptions &ir_opts = m_invocation.getIRGenOptions();
    ir_opts.OutputKind = swift::IRGenOutputKind::Module;
}

void REPL::SetupImporters()
{
    SetCurrentLoggingArea(LoggingArea::Importer);
    swift::DependencyTracker *tracker = nullptr;
    std::string module_cache_path;
    swift::ClangImporterOptions &clang_importer_opts =
        m_invocation.getClangImporterOptions();
    clang_importer_opts.OverrideResourceDir = SWIFT_CLANG_RESOURCE_DIR;
    std::unique_ptr<swift::ClangImporter> clang_importer;
    if(!clang_importer_opts.OverrideResourceDir.empty())
    {
        clang_importer = swift::ClangImporter::create(*m_ast_ctx, clang_importer_opts);
        if(!clang_importer)
        {
            Log("Failed to create ClangImporter", LoggingPriority::Error);
        }
        else
        {
            clang_importer->addSearchPath(SWIFT_SHIMS_RESOURCE_DIR, /* isFramework */ false, /* isSystem */ true);
            module_cache_path = swift::getModuleCachePathFromClang(clang_importer->getClangInstance());
            Log("Module Cache Path: " + module_cache_path);
        }
    }
    if(module_cache_path.empty())
    {
        llvm::SmallString<0> path;
        std::error_code ec = llvm::sys::fs::createUniqueDirectory("ModuleCache", path);
        if(!ec)
            module_cache_path = path.str();
        else
            module_cache_path = DEFAULT_MODULE_CACHE_PATH;
    }
    constexpr swift::ModuleLoadingMode loading_mode = swift::ModuleLoadingMode::PreferSerialized;
    llvm::Triple triple(m_invocation.getTargetTriple());
    std::string prebuilt_module_cache_path = SWIFT_BUILTIN_MODULE_PATH;
    if(loading_mode != swift::ModuleLoadingMode::OnlySerialized)
    {
        std::unique_ptr<swift::ModuleLoader> parseable_module_loader(
            swift::ParseableInterfaceModuleLoader::create(*m_ast_ctx,
                                                          module_cache_path,
                                                          prebuilt_module_cache_path,
                                                          tracker,
                                                          loading_mode)
            );
        if(parseable_module_loader)
            m_ast_ctx->addModuleLoader(std::move(parseable_module_loader));
    }
    std::unique_ptr<swift::ModuleLoader> serialized_module_loader(
        swift::SerializedModuleLoader::create(*m_ast_ctx, tracker, loading_mode)
        );

    if(serialized_module_loader)
        m_ast_ctx->addModuleLoader(std::move(serialized_module_loader));

    if(clang_importer)
        m_ast_ctx->addModuleLoader(std::move(clang_importer), /* isClang = */ true);

    // NOTE(sasha): LLDB installs a DWARF importer here. We don't care about that (or do we?)
}
