#include <llvm/ADT/Any.h>
#include <llvm/Analysis/LazyCallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <cerrno>
#include <cstdio>
#include <set>
#include <string>
#include <unistd.h>

using namespace llvm;

static const Module *gCurrMod = nullptr;
static std::string gIrName;
static unsigned gPassNumber = 0;
static unsigned gSnapshotCount = 0;
static bool gInputFailed = false;
std::set<unsigned> gPrintPassNums;

static bool isAdaptorPass(StringRef PassName) {
    return PassName.contains("PassManager<") ||
           PassName.contains("Adaptor") ||
           PassName.contains("Wrapper") ||
           PassName == "DevirtSCCRepeatedPass";
}

static const Module *getModule(const Any &IR) {
    if (auto *M = any_cast<const Module *>(&IR)) return *M;
    if (auto *F = any_cast<const Function *>(&IR)) return (*F)->getParent();
    if (auto *L = any_cast<const Loop *>(&IR))
        return (*L)->getHeader()->getModule();
    if (auto *SCC = any_cast<const LazyCallGraph::SCC *>(&IR))
        for (const auto &N : **SCC) return N.getFunction().getParent();
    if (auto *MF = any_cast<const MachineFunction *>(&IR))
        return (*MF)->getFunction().getParent();
    return nullptr;
}

static bool readControlRecord() {
    unsigned char Byte;
    for (;;) {
        ssize_t N = ::read(STDIN_FILENO, &Byte, 1);
        if (N == 1) {
            if (Byte == 0x1e) return true;
            errs() << "opt-pipe2: expected RS (0x1e) between snapshots, got byte 0x"
                   << format_hex_no_prefix(Byte, 2) << "\n";
            gInputFailed = true;
            return false;
        }
        if (N == 0) {
            errs() << "opt-pipe2: unexpected EOF while waiting for snapshot RS\n";
            gInputFailed = true;
            return false;
        }
        if (errno != EINTR) {
            errs() << "opt-pipe2: failed reading snapshot RS: " << strerror(errno)
                   << "\n";
            gInputFailed = true;
            return false;
        }
    }
}

static void printModule(const char *PassName, unsigned PassNum) {
    if (!gCurrMod || gInputFailed) return;
    if (gSnapshotCount != 0 && !readControlRecord()) return;

    std::string IRText;
    raw_string_ostream OS(IRText);
    gCurrMod->print(OS, nullptr, false, false);
    OS.flush();

    fprintf(stdout, "; *** IR after pass #%u %s %s ***\n", PassNum, PassName,
            gIrName.c_str());
    fprintf(stdout, "%s", IRText.c_str());
    fputc(0x1e, stdout);
    fflush(stdout);
    ++gSnapshotCount;
}

void saveBeforeIR(const char *PassName, const Any &IR) {
    if (isAdaptorPass(PassName)) return;
    const Module *M = getModule(IR);
    if (!M) return;
    gCurrMod = M;
    gIrName.clear();
    if (any_cast<const Module *>(&IR)) {
        gIrName = "Module";
    } else if (auto *FA = any_cast<const Function *>(&IR)) {
        gIrName = "function:" + (*FA)->getName().str();
    } else if (auto *LA = any_cast<const Loop *>(&IR)) {
        std::string Header;
        raw_string_ostream OS(Header);
        (*LA)->getHeader()->printAsOperand(OS, false);
        gIrName = "loop:" + Header + " function:" +
                  (*LA)->getHeader()->getParent()->getName().str();
    } else if (auto *SA = any_cast<const LazyCallGraph::SCC *>(&IR)) {
        gIrName = "scc:";
        bool First = true;
        for (const auto &N : **SA) {
            if (!N.getFunction().isDeclaration()) {
                if (!First) gIrName += ",";
                gIrName += N.getFunction().getName().str();
                First = false;
            }
        }
    } else if (auto *MF = any_cast<const MachineFunction *>(&IR)) {
        gIrName = "machine:" + (*MF)->getFunction().getName().str();
    }
}

void diffAfterIR(const char *PassName, const Any &IR) {
    if (isAdaptorPass(PassName)) return;
    unsigned PassNum = gPassNumber++;
    if (gCurrMod && gPrintPassNums.count(PassNum)) printModule(PassName, PassNum);
    gCurrMod = nullptr;
}

void diffAfterIRInvalidated(const char *PassName) {
    if (isAdaptorPass(PassName)) return;
    unsigned PassNum = gPassNumber++;
    if (gCurrMod && gPrintPassNums.count(PassNum)) printModule(PassName, PassNum);
    gCurrMod = nullptr;
}

bool optPipe2InputFailed() { return gInputFailed; }
