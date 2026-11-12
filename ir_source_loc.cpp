#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <system_error>

using namespace llvm;

static const DISubprogram *
getSubprogram(const Function &F,
              const StringMap<const DISubprogram *> &SubprogramsByName) {
    if (const DISubprogram *SP = F.getSubprogram())
        return SP;

    for (const BasicBlock &BB : F)
        for (const Instruction &I : BB)
            if (const DILocation *Loc = I.getDebugLoc())
                return Loc->getScope()->getSubprogram();

    auto It = SubprogramsByName.find(F.getName());
    return It == SubprogramsByName.end() ? nullptr : It->second;
}

static SmallString<256> getSourcePath(const DIFile &File) {
    StringRef Filename = File.getFilename();
    if (Filename.empty())
        return SmallString<256>();

    SmallString<256> Path;
    if (sys::path::is_absolute(Filename)) {
        Path = Filename;
    } else if (!File.getDirectory().empty()) {
        Path = File.getDirectory();
        sys::path::append(Path, Filename);
    } else {
        Path = Filename;
    }

    sys::path::remove_dots(Path, true);
    return Path;
}

int main(int argc, char **argv) {
    InitLLVM X(argc, argv);

    ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr = MemoryBuffer::getSTDIN();
    if (!BufferOrErr) {
        errs() << argv[0] << ": failed to read stdin: "
               << BufferOrErr.getError().message() << '\n';
        return 1;
    }

    LLVMContext Context;
    SMDiagnostic Err;
    std::unique_ptr<Module> M =
        parseIR((*BufferOrErr)->getMemBufferRef(), Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }

    DebugInfoFinder Finder;
    Finder.processModule(*M);
    StringMap<const DISubprogram *> SubprogramsByName;
    for (const DISubprogram *SP : Finder.subprograms()) {
        StringRef Name = SP->getLinkageName();
        if (Name.empty())
            Name = SP->getName();
        if (Name.empty())
            continue;

        auto [It, Inserted] = SubprogramsByName.try_emplace(Name, SP);
        if (!Inserted && It->second != SP)
            It->second = nullptr;
    }

    for (const Function &F : *M) {
        if (F.isDeclaration())
            continue;

        outs() << F.getName() << '\t';

        const DISubprogram *SP = getSubprogram(F, SubprogramsByName);
        const DIFile *File = SP ? SP->getFile() : nullptr;
        SmallString<256> Path = File ? getSourcePath(*File) : SmallString<256>();
        if (Path.empty())
            outs() << "<unknown>\t0\n";
        else
            outs() << Path << '\t' << SP->getLine() << '\n';
    }

    return 0;
}
