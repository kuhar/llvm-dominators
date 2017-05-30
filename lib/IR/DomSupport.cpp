//===- DomSupport.cpp - Dominators Support and Testing --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/DomSupport.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/TypeBuilder.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <sstream>

using namespace llvm;

void llvm::connect(BasicBlock *From, BasicBlock *To) {
  auto *IntTy =
      IntegerType::get(From->getParent()->getParent()->getContext(), 32);
  if (!From->getTerminator()) {
    IRBuilder<> IRB(From);
    IRB.CreateSwitch(ConstantInt::get(IntTy, 0), To);
    return;
  }

  SwitchInst *SI = cast<SwitchInst>(From->getTerminator());
  const auto Last = SI->getNumCases();

  auto *IntVal = ConstantInt::get(IntTy, Last);
  SI->addCase(IntVal, To);
}

void llvm::disconnect(BasicBlock *From, BasicBlock *To) {
  dbgs() << "Deleting BB arc " << From->getName() << " -> "
         << To->getName() << "\n";
  dbgs().flush();
  SwitchInst *SI = cast<SwitchInst>(From->getTerminator());

  if (SI->getNumCases() == 0) {
    SI->eraseFromParent();
    return;
  }

  if (SI->getDefaultDest() == To) {
    auto FirstC = SI->case_begin();
    SI->setDefaultDest(FirstC->getCaseSuccessor());
    SI->removeCase(FirstC);
    return;
  }

  for (auto CIt = SI->case_begin(); CIt != SI->case_end(); ++CIt)
    if (CIt->getCaseSuccessor() == To) {
      SI->removeCase(CIt);
      return;
    }
}

BasicBlock *InputGraph::toCFG() {
  cfg = make_unique<GraphCFG>();
  GraphCFG &CFG = *cfg;
  BasicBlock *EntryBB = nullptr;
  std::vector<BasicBlock *> Blocks(nodesNum);

  auto MakeBB = [&](StringRef name) -> BasicBlock * {
    return BasicBlock::Create(CFG.context, name, CFG.function);
  };

  auto MakeName = [](StringRef prefix, unsigned num) {
    std::string buffer;
    raw_string_ostream OS(buffer);
    OS << prefix << num;
    OS.flush();
    return OS.str();
  };

  EntryBB = Blocks[entry - 1] = MakeBB(MakeName("entry_n_", entry));
  CFG.numToBB[entry] = EntryBB;

  for (unsigned i = 1; i <= nodesNum; ++i)
    if (i != entry) {
      Blocks[i - 1] = MakeBB(MakeName("n_", i));
      CFG.numToBB[i] = Blocks[i - 1];
    }

  for (const auto &A : arcs)
    connect(Blocks[A.first - 1], Blocks[A.second - 1]);

  return EntryBB;
}

Optional<InputGraph::CFGUpdate> InputGraph::applyUpdate() {
  if (updateIdx == updates.size())
    return None;

  auto Next = updates[updateIdx++];
  auto A = cfg->getArc(Next.arc);
  if (Next.action == InputGraph::Op::Insert)
    connect(A.first, A.second);
  else
    disconnect(A.first, A.second);

  return CFGUpdate{Next.action, A};
}

Optional<InputGraph> InputGraph::readFromFile(const std::string& filename) {
  dbgs() << "Reading input graph: " << filename << "\n";
  std::ifstream IFS(filename);
  dbgs() << IFS.good() << "\n";
  InputGraph Graph;

  for (std::string Line; std::getline(IFS, Line) && !Line.empty();) {
    std::istringstream ISS(Line);
    char Action;
    ISS >> Action;
    switch (Action) {
      default:
        llvm_unreachable("Unknown action");
      case 'p': {
        assert(Graph.nodesNum == 0 && "Double init?");
        unsigned nodesNum, arcsNum, entry, dummy;
        if (!(ISS >> nodesNum >> arcsNum >> entry >> dummy))
          llvm_unreachable("Parse error");
        Graph.nodesNum = nodesNum;
        Graph.arcs.reserve(arcsNum);
        Graph.entry = entry;
      } break;
      case 'a': {
        unsigned x, y;
        if (!(ISS >> x >> y))
          llvm_unreachable("Parse error");
        Graph.arcs.push_back({x, y});
      } break;
      case 'e':
        break;
      case 'i': {
        unsigned x, y;
        if (!(ISS >> x >> y))
          llvm_unreachable("Parse error");
        assert(x <= Graph.nodesNum);
        assert(y <= Graph.nodesNum);
        Graph.updates.push_back({InputGraph::Op::Insert, {x, y}});
      } break;
      case 'd': {
        unsigned x, y;
        if (!(ISS >> x >> y))
          llvm_unreachable("Parse error");
        assert(x <= Graph.nodesNum);
        assert(y <= Graph.nodesNum);
        Graph.updates.push_back({InputGraph::Op::Delete, {x, y}});
      } break;
    }
  }

  return std::move(Graph);
}
