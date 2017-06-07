//===- NewDominators.h - Dominator Info Calculation -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the DominatorTree class, which provides fast and efficient
// dominance queries.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_NEW_DOMINATORS_H
#define LLVM_IR_NEW_DOMINATORS_H

#include <queue>
#include <utility>
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

namespace llvm {

class DominatorTree;
class Function;
class Instruction;
class Module;
class raw_ostream;

using Node = BasicBlock *;
using Index = unsigned;

struct NodeByName {
  bool operator()(const Node first, const Node second) const {
    const auto Cmp = first->getName().compare_numeric(second->getName());
    if (Cmp == 0) return less{}(first, second);

    return Cmp < 0;
  }
};

class NewDomTree {
public:
  NewDomTree(Node Root) : root(Root) { computeReachableDominators(root, 0); }

  bool contains(Node N) const;
  Node getIDom(Node N) const;
  Index getLevel(Node N) const;
  Node findNCA(Node First, Node Second) const;

  bool dominates(Node Src, Node Dst) const;

  void insertArc(Node From, Node To);
  void deleteArc(Node From, Node To);

  void toOldDT(DominatorTree& DT) const;

  enum Verification : unsigned {
    None    = 0,
    Basic   = 1,
    CFG     = 2,
    Sibling = 4,
    OldDT   = 8,
    Normal  = unsigned(Basic) | unsigned(CFG) | unsigned(OldDT),
    Full    = unsigned(Basic) | unsigned(CFG) | unsigned(Sibling) |
              unsigned(OldDT)
  };

  bool verify(Verification VerificationLevel = Verification::Basic) const;
  bool verifyWithOldDT() const;
  bool verifyNCA() const;
  bool verifyLevels() const;
  bool verifyReachability() const;
  bool verifyParentProperty() const;
  bool verifySiblingProperty() const;

  void print(raw_ostream &OS) const;
  void dump() const { print(dbgs()); }
  void dumpIDoms(raw_ostream &OS = dbgs()) const;
  void dumpLevels(raw_ostream &OS = dbgs()) const;
  void addDebugInfoToIR();
  void viewCFG() const { root->getParent()->viewCFG(); }
  void dumpLegacyDomTree() const;

 private:
  Node root = nullptr;
  DenseMap<Node, Node> idoms;
  DenseMap<Node, Node> rdoms;
  DenseMap<Node, Index> levels;
  DenseMap<Node, Node> preorderParents;
  DenseMap<Node, SmallVector<Node, 6>> children;
  mutable DenseMap<Node, std::pair<Index, Index>> inOutNums;
  mutable bool isInOutValid = false;

  void computeReachableDominators(Node Root, Index MinLevel);
  void computeUnreachableDominators(
      Node Root, Node Incoming,
      SmallVectorImpl<std::pair<Node, Node>> &DiscoveredConnectingArcs);

  struct DFSNodeInfo {
    SmallVector<Node, 8> Predecessors;
    Index Num;
    Node Parent;
  };

  struct DFSResult {
    Index nextDFSNum = 0;
    std::vector<Node> numToNode;
    DenseMap<Node, DFSNodeInfo> NodeToInfo;

    void dumpDFSNumbering(raw_ostream &OS = dbgs()) const;
  };

  template <typename DescendCondition>
  static DFSResult runDFS(Node Start, DescendCondition Condition);

  void semiNCA(DFSResult &DFS, Node Root, Index MinLevel,
               Node AttachTo = nullptr);

  struct InsertionInfo {
    using BucketElementTy = std::pair<Index, Node>;
    struct DecreasingLevel {
      bool operator()(const BucketElementTy &First,
                      const BucketElementTy &Second) const {
        return First.first > Second.first;
      }
    };

    std::priority_queue<BucketElementTy, SmallVector<BucketElementTy, 8>,
                        DecreasingLevel>
        bucket;
    DenseSet<Node> affected;
    DenseSet<Node> visited;
    SmallVector<Node, 8> affectedQueue;
    SmallVector<Node, 8> visitedNotAffectedQueue;
  };

  bool hasChild(Node N, Node Child) const;
  void addChild(Node N, Node Child);
  void removeChild(Node N, Node Child);
  void setIDom(Node N, Node NewIDom);
  Node getSDomCandidate(Node Start, Node Pred, DFSResult &DFS,
                        DenseMap<Node, Node> &Labels);

  void insertUnreachable(Node From, Node To);
  void insertReachable(Node From, Node To);
  void visitInsertion(Node N, Index RootLevel, Node NCA, InsertionInfo &II);
  void updateInsertion(Node NCA, InsertionInfo &II);
  void updateLevels(InsertionInfo &II);

  bool isReachableFromIDom(Node N);
  void deleteReachable(Node From, Node To);
  void deleteUnreachable(Node To);

  void recomputeInOutNums() const;

  using ChildrenTy = DenseMap<Node, SmallVector<Node, 8>>;
  void printImpl(raw_ostream &OS, Node N, const ChildrenTy &Children,
                 std::set<Node, NodeByName> &ToPrint) const;
};

template <typename DescendCondition>
NewDomTree::DFSResult NewDomTree::runDFS(Node Start,
                                         DescendCondition Condition) {
  DFSResult Res;
  Res.nextDFSNum = 0;
  DenseSet<Node> Visited;
  SmallVector<Node, 16> WorkList;

  Res.NodeToInfo[Start].Parent = nullptr;
  WorkList.push_back(Start);

  // Compute preorder numbers nad parents.
  while (!WorkList.empty()) {
    BasicBlock *BB = WorkList.back();
    WorkList.pop_back();
    if (Visited.count(BB) != 0) continue;

    auto &BBInfo = Res.NodeToInfo[BB];
    BBInfo.Num = Res.nextDFSNum;
    Res.numToNode.push_back(BB);
    ++Res.nextDFSNum;
    Visited.insert(BB);
    for (auto *Succ : reverse(successors(BB))) {
      auto &SuccInfo = Res.NodeToInfo[Succ];
      if (Succ != BB)
        SuccInfo.Predecessors.push_back(BB);
      if (Visited.count(Succ) == 0)
        if (Condition(BB, Succ)) {
          WorkList.push_back(Succ);
          SuccInfo.Parent = BB;
        }
    }
  }

  return Res;
}

}  // end namespace llvm

#endif  // LLVM_IR_NEW_DOMINATORS_H
