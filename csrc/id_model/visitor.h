#pragma once

#include <disjoint_set.h>
#include <id_model/id_graph.h>
#include <ir_all_nodes.h>

namespace nvfuser {

// Iterates through an IterDomain Graph in topological order, calling handle on
// all Id and all Expr groups in a forward topological order.
//
// Warning: Expr groups that have an input and output in the same IdGroup are
// ignored.
//
// Warning: This is not a great iterator if there's a desire to minimize paths
// traveled to simply visit all IdGroups in order. See ExprsBetween to see how
// we might minimize paths.
class TORCH_CUDA_CU_API IdGraphVisitor {
 protected:
  // If sub_selection is assumed to be a set of iter domains by which form a
  // sub-regrion of the IdGraph provided. Only that sub-region will be visited.
  IdGraphVisitor(
      const IdGraph& id_graph,
      const VectorOfUniqueEntries<IterDomain*> sub_selection = {})
      : id_graph_(id_graph), sub_selection_(sub_selection) {}

  virtual void handle(IdGroup id_group) = 0;
  virtual void handle(ExprGroup expr_group) = 0;

  void traverse();

  const IdGraph& graph() {
    return id_graph_;
  };

  IdGraphVisitor() = delete;

  IdGraphVisitor(const IdGraphVisitor& other) = default;
  IdGraphVisitor& operator=(const IdGraphVisitor& other) = delete;

  IdGraphVisitor(IdGraphVisitor&& other) = default;
  IdGraphVisitor& operator=(IdGraphVisitor&& other) = delete;

  virtual ~IdGraphVisitor() = default;

 private:
  const IdGraph& id_graph_;
  const VectorOfUniqueEntries<IterDomain*> sub_selection_;
};

// Statement sorting based on IdGraphVisitor, see warnings to IdGraph Visitor.
class IdGraphStmtSort : public IdGraphVisitor {
 public:
  IdGraphStmtSort(
      const IdGraph& id_graph,
      const VectorOfUniqueEntries<IterDomain*> sub_selection = {})
      : IdGraphVisitor(id_graph, sub_selection) {
    IdGraphVisitor::traverse();
  }

  ExprGroups exprs() {
    return sorted_exprs;
  }

  IdGroups ids() {
    return sorted_ids;
  }

  ~IdGraphStmtSort() override = default;

 protected:
  using IdGraphVisitor::handle;
  void handle(IdGroup id_group) override {
    sorted_ids.pushBack(id_group);
  }

  void handle(ExprGroup expr_group) override {
    sorted_exprs.pushBack(expr_group);
  }

  ExprGroups sorted_exprs;
  IdGroups sorted_ids;
};

}