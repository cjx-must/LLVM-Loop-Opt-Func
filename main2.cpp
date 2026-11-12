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
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <cerrno>
#include <set>
#include <string>
#include <unistd.h>

using namespace llvm;

void saveBeforeIR(const char *PassName, const Any &IR);
void diffAfterIR(const char *PassName, const Any &IR);
void diffAfterIRInvalidated(const char *PassName);
bool optPipe2InputFailed();
extern std::set<unsigned> gPrintPassNums;

static void parsePassNumbers(const char *Str) {
    gPrintPassNums.clear();
    std::string S(Str);
    size_t Pos = 0;
    while (Pos < S.size()) {
        size_t Next = S.find(',', Pos);
        if (Next == std::string::npos) Next = S.size();
        std::string Token = S.substr(Pos, Next - Pos);
        if (!Token.empty()) gPrintPassNums.insert(std::stoul(Token));
        Pos = Next + 1;
    }
}

static bool readInitialIR(std::string &Text) {
    unsigned char Byte;
    for (;;) {
        ssize_t N = ::read(STDIN_FILENO, &Byte, 1);
        if (N == 1) {
            if (Byte == 0x1e) {
                if (Text.empty()) {
                    errs() << "opt-pipe2: initial IR is empty before RS\n";
                    return false;
                }
                return true;
            }
            Text.push_back(static_cast<char>(Byte));
            continue;
        }
        if (N == 0) {
            errs() << "opt-pipe2: unexpected EOF before initial IR RS\n";
            return false;
        }
        if (errno != EINTR) {
            errs() << "opt-pipe2: failed reading initial IR: " << strerror(errno)
                   << "\n";
            return false;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        errs() << "Usage: opt-pipe2 <opt-level> <pass-numbers>\n"
               << "  opt-level:     -O0|-O1|-O2|-O3|-Os|-Oz\n"
               << "  pass-numbers:  comma-separated list, e.g. 3,7,15\n";
        return 1;
    }
    LLVMContext Context;
    SMDiagnostic Err;
    std::string OptLevel = argv[1];
    OptimizationLevel MidLevel;
    CodeGenOptLevel CGOptLevel;
    if      (OptLevel == "-O0") { MidLevel = OptimizationLevel::O0; CGOptLevel = CodeGenOptLevel::None; }
    else if (OptLevel == "-O1") { MidLevel = OptimizationLevel::O1; CGOptLevel = CodeGenOptLevel::Less; }
    else if (OptLevel == "-O2") { MidLevel = OptimizationLevel::O2; CGOptLevel = CodeGenOptLevel::Default; }
    else if (OptLevel == "-Os") { MidLevel = OptimizationLevel::Os; CGOptLevel = CodeGenOptLevel::Default; }
    else if (OptLevel == "-Oz") { MidLevel = OptimizationLevel::Oz; CGOptLevel = CodeGenOptLevel::Default; }
    else if (OptLevel == "-O3") { MidLevel = OptimizationLevel::O3; CGOptLevel = CodeGenOptLevel::Aggressive; }
    else { errs() << "opt-pipe2: unknown level '" << OptLevel << "'\n"; return 1; }
    parsePassNumbers(argv[2]);

    std::string IRText;
    if (!readInitialIR(IRText)) return 1;
    std::unique_ptr<MemoryBuffer> Buffer = MemoryBuffer::getMemBufferCopy(IRText, "<stdin>");
    std::unique_ptr<Module> M = parseIR(Buffer->getMemBufferRef(), Err, Context);
    if (!M) { Err.print("opt-pipe2", errs()); return 1; }

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    std::string TripleStr = M->getTargetTriple();
    if (TripleStr.empty()) TripleStr = sys::getDefaultTargetTriple();
    std::unique_ptr<TargetMachine> TM;
    std::string ErrorStr;
    if (const Target *T = TargetRegistry::lookupTarget(TripleStr, ErrorStr))
        TM.reset(T->createTargetMachine(TripleStr, "", "", TargetOptions(), {}, {}, CGOptLevel));
    else errs() << "opt-pipe2: warning: " << ErrorStr << "\n";

    PassInstrumentationCallbacks PIC;
    PIC.registerBeforeNonSkippedPassCallback([](StringRef Pass, Any IR) { saveBeforeIR(Pass.str().c_str(), IR); });
    PIC.registerAfterPassCallback([](StringRef Pass, Any IR, const PreservedAnalyses &) { diffAfterIR(Pass.str().c_str(), IR); });
    PIC.registerAfterPassInvalidatedCallback([](StringRef Pass, const PreservedAnalyses &) { diffAfterIRInvalidated(Pass.str().c_str()); });
    StandardInstrumentations SI(Context, false);
    PipelineTuningOptions PTO;
    PTO.LoopUnrolling = true;
    LoopAnalysisManager LAM; FunctionAnalysisManager FAM; CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM; MachineFunctionAnalysisManager MFAM;
    SI.registerCallbacks(PIC, &MAM);
    PassBuilder PB(TM.get(), PTO, std::nullopt, &PIC);
    AAManager AA;
    if (auto E = PB.parseAAPipeline(AA, "default")) { errs() << toString(std::move(E)) << "\n"; return 1; }
    FAM.registerPass([&] { return std::move(AA); });
    TargetLibraryInfoImpl TLII{Triple(TripleStr)};
    FAM.registerPass([&] { return TargetLibraryAnalysis(TLII); });
    PB.registerModuleAnalyses(MAM); PB.registerCGSCCAnalyses(CGAM); PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM); PB.registerMachineFunctionAnalyses(MFAM); PB.crossRegisterProxies(LAM, FAM, CGAM, MAM, &MFAM);
    ModulePassManager MPM;
    if (auto E = PB.parsePassPipeline(MPM, "default<" + OptLevel.substr(1) + ">")) { errs() << toString(std::move(E)) << "\n"; return 1; }
    MPM.run(*M, MAM);
    if (TM) {
        MachineModuleInfo MMI(TM.get()); MAM.registerPass([&] { return MachineModuleAnalysis(MMI); });
        registerCodeGenCallback(PIC, *TM); CGPassBuilderOption CGOpt = getCGPassBuilderOption(); CGOpt.DisableVerify = true;
        ModulePassManager CodegenMPM; raw_null_ostream NullStrm;
        if (auto E = TM->buildCodeGenPipeline(CodegenMPM, NullStrm, nullptr, CodeGenFileType::Null, CGOpt, &PIC)) return 1;
        CodegenMPM.run(*M, MAM);
    }
    return optPipe2InputFailed() ? 1 : 0;
}
