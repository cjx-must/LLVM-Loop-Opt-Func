#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

using namespace llvm;

void saveBeforeIR(const char *PassName, const llvm::Any &IR);
void diffAfterIR(const char *PassName, const llvm::Any &IR);
void diffAfterIRInvalidated(const char *PassName);

int main(int argc, char **argv) {
    LLVMContext Context;
    SMDiagnostic Err;

    // ---- parse optimization level ----
    std::string OptLevel = (argc > 1) ? argv[1] : "";

    OptimizationLevel MidLevel;
    CodeGenOptLevel CGOptLevel;
    if      (OptLevel == "-O0") { MidLevel = OptimizationLevel::O0; CGOptLevel = CodeGenOptLevel::None; }
    else if (OptLevel == "-O1") { MidLevel = OptimizationLevel::O1; CGOptLevel = CodeGenOptLevel::Less; }
    else if (OptLevel == "-O2") { MidLevel = OptimizationLevel::O2; CGOptLevel = CodeGenOptLevel::Default; }
    else if (OptLevel == "-Os") { MidLevel = OptimizationLevel::Os; CGOptLevel = CodeGenOptLevel::Default; }
    else if (OptLevel == "-Oz") { MidLevel = OptimizationLevel::Oz; CGOptLevel = CodeGenOptLevel::Default; }
    else if (OptLevel == "-O3") { MidLevel = OptimizationLevel::O3; CGOptLevel = CodeGenOptLevel::Aggressive; }
    else {
        errs() << "opt-pipe0: unknown level '" << OptLevel
               << "', use -O0|-O1|-O2|-O3|-Os|-Oz\n";
        return 1;
    }

    // ---- parse IR ----
    std::unique_ptr<Module> M = parseIRFile("-", Err, Context);
    if (!M) {
        Err.print("opt-pipe0", errs());
        return 1;
    }

    // ---- target setup (for TTI in mid-end) ----
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    std::string TripleStr = M->getTargetTriple();
    if (TripleStr.empty())
        TripleStr = sys::getDefaultTargetTriple();

    std::unique_ptr<TargetMachine> TM;
    std::string ErrorStr;
    if (const Target *T = TargetRegistry::lookupTarget(TripleStr, ErrorStr))
        TM.reset(T->createTargetMachine(TripleStr, "", "", TargetOptions(),
                                        {}, {}, CGOptLevel));
    else
        errs() << "opt-pipe0: warning: " << ErrorStr << "\n";

    // ---- mid-end pipeline ----
    PassInstrumentationCallbacks PIC;
    PIC.registerBeforeNonSkippedPassCallback(
        [](StringRef Pass, Any IR) { 
            saveBeforeIR(Pass.str().c_str(), IR); 
        });
    PIC.registerAfterPassCallback(
        [](StringRef Pass, Any IR, const PreservedAnalyses &) {
            diffAfterIR(Pass.str().c_str(), IR);
        });
    PIC.registerAfterPassInvalidatedCallback(
        [](StringRef Pass, const PreservedAnalyses &) {
            diffAfterIRInvalidated(Pass.str().c_str());
        });

    StandardInstrumentations SI(Context, /*DebugLogging=*/false);

    PipelineTuningOptions PTO;
    PTO.LoopUnrolling = true;

    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    MachineFunctionAnalysisManager MFAM;

    SI.registerCallbacks(PIC, &MAM);

    PassBuilder PB(TM.get(), PTO, std::nullopt, &PIC);

    AAManager AA;
    if (auto Err = PB.parseAAPipeline(AA, "default")) {
        errs() << "opt-pipe0: AA pipeline error: "
               << toString(std::move(Err)) << "\n";
        return 1;
    }
    FAM.registerPass([&] { return std::move(AA); });

    TargetLibraryInfoImpl TLII{Triple(TripleStr)};
    FAM.registerPass([&] { return TargetLibraryAnalysis(TLII); });

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.registerMachineFunctionAnalyses(MFAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM, &MFAM);

    ModulePassManager MPM;
    if (auto Err = PB.parsePassPipeline(MPM, "default<" + OptLevel.substr(1) + ">")) {
        errs() << "opt-pipe0: pipeline error: "
               << toString(std::move(Err)) << "\n";
        return 1;
    }

    MPM.run(*M, MAM);

    // ---- CodeGen pipeline (new PM) ----
    if (!TM) {
        errs() << "opt-pipe0: warning: no target machine, skipping codegen\n";
    } else {
        MachineModuleInfo MMI(TM.get());
        MAM.registerPass([&] { return MachineModuleAnalysis(MMI); });
        registerCodeGenCallback(PIC, *TM);

        CGPassBuilderOption CGOpt = getCGPassBuilderOption();
        CGOpt.DisableVerify = true;

        ModulePassManager codegenMPM;
        raw_null_ostream nullStrm;
        if (auto Err = TM->buildCodeGenPipeline(codegenMPM, nullStrm, nullptr,
                                                CodeGenFileType::Null,
                                                CGOpt, &PIC)) {
            errs() << "opt-pipe0: codegen pipeline error\n";
            return 1;
        }
        codegenMPM.run(*M, MAM);
    }

    return 0;
}
