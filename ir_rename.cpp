// g++ ir_rename.cpp $(llvm-config --cxxflags --ldflags --system-libs --libs core irreader support) -O2 -o ir_rename
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>

using namespace llvm;

void renameGlobal(Module &M, const std::string &oldName, const std::string &newName) {
    GlobalValue *GV = M.getNamedValue(oldName);
    if (!GV) {
        errs() << "Error: Global value " << oldName << " not found!\n";
        std::exit(1);
    }
    GV->setName(newName);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        errs() << "Usage: " << argv[0] << " <old_name> <new_name>\n";
        return 1;
    }

    LLVMContext Context;
    SMDiagnostic Err;

    // 1. 从 stdin 读取 IR
    auto BufferOrErr = MemoryBuffer::getSTDIN();
    if (!BufferOrErr) {
        errs() << "Error: Failed to read from stdin\n";
        return 1;
    }

    std::unique_ptr<MemoryBuffer> Buffer = std::move(*BufferOrErr);

    // 2. 解析 IR
    std::unique_ptr<Module> M = parseIR(Buffer->getMemBufferRef(), Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }

    // 3. 重命名全局值
    renameGlobal(*M, argv[1], argv[2]);

    // 4. 输出 LLVM IR 到 stdout（文本格式）
    M->print(outs(), nullptr);

    return 0;
}