// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "toolchain/lowering/lowering_context.h"

#include "common/vlog.h"
#include "toolchain/semantics/semantics_ir.h"
#include "toolchain/semantics/semantics_node_kind.h"

namespace Carbon {

LoweringContext::LoweringContext(llvm::LLVMContext& llvm_context,
                                 llvm::StringRef module_name,
                                 const SemanticsIR& semantics_ir,
                                 llvm::raw_ostream* vlog_stream)
    : llvm_context_(&llvm_context),
      llvm_module_(std::make_unique<llvm::Module>(module_name, llvm_context)),
      builder_(llvm_context),
      semantics_ir_(&semantics_ir),
      vlog_stream_(vlog_stream) {
  CARBON_CHECK(!semantics_ir.has_errors())
      << "Generating LLVM IR from invalid SemanticsIR is unsupported.";
}

auto LoweringContext::Run() -> std::unique_ptr<llvm::Module> {
  CARBON_CHECK(llvm_module_) << "Run can only be called once.";

  // Lower types.
  auto types = semantics_ir_->types();
  types_.resize_for_overwrite(types.size());
  for (int i = 0; i < static_cast<int>(types.size()); ++i) {
    types_[i] = BuildType(types[i]);
  }

  // Lower function declarations.
  functions_.resize_for_overwrite(semantics_ir_->functions_size());
  for (int i = 0; i < semantics_ir_->functions_size(); ++i) {
    functions_[i] = BuildFunctionDeclaration(SemanticsFunctionId(i));
  }

  // TODO: Lower global variable declarations.

  // Lower function definitions.
  for (int i = 0; i < semantics_ir_->functions_size(); ++i) {
    BuildFunctionDefinition(SemanticsFunctionId(i));
  }

  // TODO: Lower global variable initializers.

  return std::move(llvm_module_);
}

auto LoweringContext::BuildFunctionDeclaration(SemanticsFunctionId function_id)
    -> llvm::Function* {
  auto function = semantics_ir().GetFunction(function_id);

  // TODO: Lower type information for the arguments prior to building args.
  auto param_refs = semantics_ir().GetNodeBlock(function.param_refs_id);
  llvm::SmallVector<llvm::Type*> args;
  args.resize_for_overwrite(param_refs.size());
  for (int i = 0; i < static_cast<int>(param_refs.size()); ++i) {
    args[i] = GetType(semantics_ir().GetNode(param_refs[i]).type_id());
  }

  llvm::Type* return_type = GetType(function.return_type_id.is_valid()
                                        ? function.return_type_id
                                        : semantics_ir().empty_tuple_type_id());
  llvm::FunctionType* function_type =
      llvm::FunctionType::get(return_type, args, /*isVarArg=*/false);
  auto* llvm_function = llvm::Function::Create(
      function_type, llvm::Function::ExternalLinkage,
      semantics_ir().GetString(function.name_id), llvm_module());

  // Set parameter names.
  for (int i = 0; i < static_cast<int>(param_refs.size()); ++i) {
    auto [param_name_id, _] =
        semantics_ir().GetNode(param_refs[i]).GetAsBindName();
    llvm_function->getArg(i)->setName(semantics_ir().GetString(param_name_id));
  }

  return llvm_function;
}

auto LoweringContext::BuildFunctionDefinition(SemanticsFunctionId function_id)
    -> void {
  auto function = semantics_ir().GetFunction(function_id);
  auto body_id = function.body_id;
  if (!body_id.is_valid()) {
    // Function is probably defined in another file; not an error.
    return;
  }
  auto* llvm_function = GetFunction(function_id);

  // Create a new basic block to start insertion into.
  builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_context(), "entry",
                                                   GetFunction(function_id)));
  CARBON_CHECK(locals_.empty());

  // Add parameters to locals.
  auto param_refs = semantics_ir().GetNodeBlock(function.param_refs_id);
  for (int i = 0; i < static_cast<int>(param_refs.size()); ++i) {
    auto param_storage =
        semantics_ir().GetNode(param_refs[i]).GetAsBindName().second;
    CARBON_CHECK(
        locals_.insert({param_storage, llvm_function->getArg(i)}).second)
        << "Duplicate param: " << param_refs[i];
  }

  CARBON_VLOG() << "Lowering " << body_id << "\n";
  for (const auto& node_id : semantics_ir_->GetNodeBlock(body_id)) {
    auto node = semantics_ir_->GetNode(node_id);
    CARBON_VLOG() << "Lowering " << node_id << ": " << node << "\n";
    switch (node.kind()) {
#define CARBON_SEMANTICS_NODE_KIND(Name)        \
  case SemanticsNodeKind::Name:                 \
    LoweringHandle##Name(*this, node_id, node); \
    break;
#include "toolchain/semantics/semantics_node_kind.def"
    }
  }

  // Clear locals.
  locals_.clear();
}

auto LoweringContext::BuildType(SemanticsNodeId node_id) -> llvm::Type* {
  switch (node_id.index) {
    case SemanticsBuiltinKind::EmptyTupleType.AsInt():
      // Represent empty types as empty structs.
      // TODO: Investigate special-casing handling of these so that they can be
      // collectively replaced with LLVM's void, particularly around function
      // returns. LLVM doesn't allow declaring variables with a void type, so
      // that may require significant special casing.
      return llvm::StructType::create(
          *llvm_context_, llvm::ArrayRef<llvm::Type*>(),
          SemanticsBuiltinKind::FromInt(node_id.index).name());
    case SemanticsBuiltinKind::FloatingPointType.AsInt():
      // TODO: Handle different sizes.
      return builder_.getDoubleTy();
    case SemanticsBuiltinKind::IntegerType.AsInt():
      // TODO: Handle different sizes.
      return builder_.getInt32Ty();
  }

  auto node = semantics_ir_->GetNode(node_id);
  switch (node.kind()) {
    case SemanticsNodeKind::StructType: {
      auto refs = semantics_ir_->GetNodeBlock(node.GetAsStructType());
      llvm::SmallVector<llvm::Type*> subtypes;
      subtypes.reserve(refs.size());
      for (auto ref_id : refs) {
        auto type_id = semantics_ir_->GetNode(ref_id).type_id();
        // TODO: Handle recursive types. The restriction for builtins prevents
        // recursion while still letting them cache.
        CARBON_CHECK(type_id.index < SemanticsBuiltinKind::ValidCount)
            << type_id;
        subtypes.push_back(GetType(type_id));
      }
      return llvm::StructType::create(*llvm_context_, subtypes,
                                      "StructLiteralType");
    }
    default: {
      CARBON_FATAL() << "Cannot use node as type: " << node_id;
    }
  }
}

auto LoweringContext::GetLocalLoaded(SemanticsNodeId node_id) -> llvm::Value* {
  auto* value = GetLocal(node_id);
  if (llvm::isa<llvm::AllocaInst, llvm::GetElementPtrInst>(value)) {
    auto* load_type = GetType(semantics_ir().GetNode(node_id).type_id());
    return builder().CreateLoad(load_type, value);
  } else {
    // No load is needed.
    return value;
  }
}

}  // namespace Carbon
