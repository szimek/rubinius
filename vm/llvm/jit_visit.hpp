#include "instructions_util.hpp"

#include "builtin/symbol.hpp"
#include "builtin/tuple.hpp"
#include "builtin/sendsite.hpp"
#include "builtin/global_cache_entry.hpp"
#include "inline_cache.hpp"

#include "llvm/access_memory.hpp"
#include "llvm/jit_operations.hpp"
#include "llvm/inline.hpp"

#include <llvm/DerivedTypes.h>

#include <list>

namespace rubinius {

  typedef std::list<llvm::BasicBlock*> EHandlers;

  class JITFunction : public Signature {
    llvm::Function* func_;

  public:
    JITFunction(LLVMState* ls)
      : Signature(ls, Type::VoidTy)
      , func_(0)
    {}

    void resolve(const char* name, const Type* rt) {
      ret_type_ = rt;
      func_ = function(name);
    }

    Value* call(Value** start, int size, const char* inst_name, IRBuilder<>& b) {
      return b.CreateCall(func_, start, start+size, inst_name);
    }
  };

  class JITFunctions {
  public:
    JITFunction return_to_here;
    JITFunction clear_raise_value;

    JITFunctions(LLVMState* ls)
      : return_to_here(ls)
      , clear_raise_value(ls)
    {
      return_to_here
        << "VM"
        << "CallFrame";

      return_to_here.resolve("rbx_return_to_here", Type::Int1Ty);

      clear_raise_value
        << "VM";

      clear_raise_value.resolve("rbx_clear_raise_value", ls->object());
    }
  };

  class JITVisit : public VisitInstructions<JITVisit>, public JITOperations {
    JITFunctions f;
    BlockMap& block_map_;

    bool allow_private_;
    opcode call_flags_;

    // Cached Function*s
    llvm::Function* rbx_simple_send_;
    llvm::Function* rbx_simple_send_private_;

    // bail out destinations
    llvm::BasicBlock* bail_out_;
    llvm::BasicBlock* bail_out_fast_;

    EHandlers exception_handlers_;

    Value* method_entry_;
    Value* args_;

    Value* ip_pos_;

    Value* vars_;
    bool use_full_scope_;

    bool is_block_;
    BasicBlock* inline_return_;
    PHINode* return_value_;

    Value* global_serial_pos;

    // The single Arguments object on the stack, plus positions into it
    // that we store the call info
    Value* out_args_;
    Value* out_args_recv_;
    Value* out_args_block_;
    Value* out_args_total_;
    Value* out_args_arguments_;
    Value* out_args_array_;

    int called_args_;
    int sends_done_;

  public:

    class Unsupported {};

    void init_out_args(BasicBlock* block) {
      Instruction* term = block->getTerminator();
      assert(term);

      BasicBlock* old = current_block();
      set_block(block);

      out_args_ = b().CreateAlloca(type("Arguments"), 0, "out_args");

      out_args_recv_ = ptr_gep(out_args_, 0, "out_args_recv");
      out_args_block_= ptr_gep(out_args_, 1, "out_args_block");
      out_args_total_= ptr_gep(out_args_, 2, "out_args_total");
      out_args_arguments_ = ptr_gep(out_args_, 3, "out_args_arguments");
      out_args_array_ = ptr_gep(out_args_, 4, "out_args_array");

      term->removeFromParent();
      term->insertAfter(cast<Instruction>(out_args_array_));

      set_block(old);
    }

    JITVisit(LLVMState* ls, JITMethodInfo& info, BlockMap& bm,
             llvm::Module* mod, llvm::Function* func, llvm::BasicBlock* start,
             llvm::Value* stack, llvm::Value* call_frame,
             llvm::Value* me, llvm::Value* args,
             llvm::Value* vars, bool is_block, BasicBlock* inline_return = 0)
      : JITOperations(ls, info, mod, stack, call_frame, start, func)
      , f(ls)
      , block_map_(bm)
      , allow_private_(false)
      , call_flags_(0)
      , rbx_simple_send_(0)
      , rbx_simple_send_private_(0)
      , method_entry_(me)
      , args_(args)
      , vars_(vars)
      , use_full_scope_(false)
      , is_block_(is_block)
      , inline_return_(inline_return)
      , return_value_(0)
      , called_args_(-1)
      , sends_done_(0)
    {

      if(inline_return) {
        return_value_ = PHINode::Create(ObjType, "inline_return_val");
      }

      bail_out_ = new_block("bail_out");

      Value* call_args[] = {
        vm_,
        call_frame_
      };

      set_block(bail_out_);

      Value* isit = f.return_to_here.call(call_args, 2, "rth", b());

      BasicBlock* ret_raise_val = new_block("ret_raise_val");
      bail_out_fast_ = new_block("ret_null");

      start->moveAfter(bail_out_fast_);

      b().CreateCondBr(isit, ret_raise_val, bail_out_fast_);

      set_block(bail_out_fast_);
      if(!inline_return_) {
        if(use_full_scope_) flush_scope_to_heap(vars_);
      }

      if(inline_return_) {
        return_value_->addIncoming(Constant::getNullValue(ObjType), current_block());
        b().CreateBr(inline_return_);
      } else {
        b().CreateRet(Constant::getNullValue(ObjType));
      }

      set_block(ret_raise_val);
      Value* crv = f.clear_raise_value.call(&vm_, 1, "crv", b());
      if(!inline_return_) {
        if(use_full_scope_) flush_scope_to_heap(vars_);
      }

      if(inline_return_) {
        return_value_->addIncoming(crv, current_block());
        b().CreateBr(inline_return_);
      } else {
        b().CreateRet(crv);
      }

      set_block(start);

      ip_pos_ = b().CreateConstGEP2_32(call_frame_, 0, offset::cf_ip, "ip_pos");

      global_serial_pos = b().CreateIntToPtr(
          ConstantInt::get(IntPtrTy, (intptr_t)ls_->shared().global_serial_address()),
          PointerType::getUnqual(IntPtrTy), "cast_to_intptr");

      init_out_args(&function_->getEntryBlock());
    }

    Value* return_value() {
      return return_value_;
    }

    void use_full_scope() {
      use_full_scope_ = true;
    }

    void set_called_args(int args) {
      called_args_ = args;
    }

    int sends_done() {
      return sends_done_;
    }

    Value* scope() {
      return b().CreateLoad(
          b().CreateConstGEP2_32(call_frame_, 0, offset::cf_scope, "scope_pos"),
          "scope");
    }

    Value* top_scope() {
      return b().CreateLoad(
          b().CreateConstGEP2_32(call_frame_, 0, offset::cf_top_scope, "top_scope_pos"),
          "top_scope");
    }

    BlockMap& block_map() {
      return block_map_;
    }

    void check_for_return(Value* val) {
      BasicBlock* cont = new_block();

      Value* null = Constant::getNullValue(ObjType);

      BasicBlock* orig = current_block();

      Value* cmp = b().CreateICmpEQ(val, null, "null_check");
      BasicBlock* is_break = new_block("is_break");

      b().CreateCondBr(cmp, is_break, cont);

      /////
      set_block(is_break);

      Signature brk(ls_, Type::Int1Ty);
      brk << VMTy;
      brk << CallFrameTy;

      Value* call_args[] = {
        vm_,
        call_frame_
      };

      Value* isit = brk.call("rbx_break_to_here", call_args, 2, "bth", b());

      BasicBlock* push_break_val = new_block("push_break_val");
      BasicBlock* next = 0;

      // If there are handlers...
      if(exception_handlers_.size() > 0) {
        next = exception_handlers_.back();
      } else {
        next = bail_out_;
      }

      b().CreateCondBr(isit, push_break_val, next);

      ////
      set_block(push_break_val);

      Signature clear(ls_, ObjType);
      clear << VMTy;
      Value* crv = clear.call("rbx_clear_raise_value", &vm_, 1, "crv", b());

      b().CreateBr(cont);

      /////
      set_block(cont);

      PHINode* phi = b().CreatePHI(ObjType, "possible_break");
      phi->addIncoming(val, orig);
      phi->addIncoming(crv, push_break_val);

      stack_push(phi);
    }

    void propagate_exception() {
      // If there are handlers...
      if(exception_handlers_.size() > 0) {
        BasicBlock* handler = exception_handlers_.back();
        b().CreateBr(handler);
      } else {
        b().CreateBr(bail_out_fast_);
      }
    }

    void check_for_exception_then(Value* val, BasicBlock* cont) {
      Value* null = Constant::getNullValue(ObjType);

      Value* cmp = b().CreateICmpEQ(val, null, "null_check");

      // If there are handlers...
      if(exception_handlers_.size() > 0) {
        BasicBlock* handler = exception_handlers_.back();
        b().CreateCondBr(cmp, handler, cont);
      } else {
        b().CreateCondBr(cmp, bail_out_fast_, cont);
      }
    }

    void check_for_exception(Value* val) {
      BasicBlock* cont = new_block();
      check_for_exception_then(val, cont);
      set_block(cont);
    }

    void at_ip(int ip) {
      // Bad startup edge case
      if(ip == 0) return;

      BlockMap::iterator i = block_map_.find(ip);
      if(i != block_map_.end()) {
        JITBasicBlock& jbb = i->second;
        if(BasicBlock* next = jbb.block) {
          if(!b().GetInsertBlock()->getTerminator()) {
            b().CreateBr(next);
          }

          // std::cout << ip << ": " << jbb.sp << "\n";

          next->moveAfter(b().GetInsertBlock());

          set_block(next);
        }
        if(jbb.sp != -10) set_sp(jbb.sp);
      }

      remember_sp();

      b().CreateStore(ConstantInt::get(Type::Int32Ty, ip), ip_pos_);
    }

    // visitors.

    void visit(opcode op, opcode arg1, opcode arg2) {
      throw Unsupported();
    }

    void visit_pop() {
      stack_remove(1);
    }

    void visit_push_nil() {
      stack_push(constant(Qnil));
    }

    void visit_push_true() {
      stack_push(constant(Qtrue));
    }

    void visit_push_false() {
      stack_push(constant(Qfalse));
    }

    void visit_push_int(opcode arg) {
      stack_push(constant(Fixnum::from(arg)));
    }

    void visit_meta_push_0() {
      stack_push(constant(Fixnum::from(0)));
    }

    void visit_meta_push_1() {
      stack_push(constant(Fixnum::from(1)));
    }

    void visit_meta_push_2() {
      stack_push(constant(Fixnum::from(2)));
    }

    void visit_meta_push_neg_1() {
      stack_push(constant(Fixnum::from(-1)));
    }

    void visit_ret() {
      if(ls_->include_profiling()) {
        Value* test = b().CreateLoad(ls_->profiling(), "profiling");
        BasicBlock* end_profiling = new_block("end_profiling");
        BasicBlock* cont = new_block("continue");

        b().CreateCondBr(test, end_profiling, cont);

        set_block(end_profiling);

        Signature sig(ls_, Type::VoidTy);
        sig << PointerType::getUnqual(Type::Int8Ty);

        Value* call_args[] = {
          method_entry_
        };

        sig.call("rbx_end_profiling", call_args, 1, "", b());

        b().CreateBr(cont);

        set_block(cont);
      }

      if(inline_return_) {
        return_value_->addIncoming(stack_top(), current_block());
        b().CreateBr(inline_return_);
      } else {
        if(use_full_scope_) flush_scope_to_heap(vars_);
        b().CreateRet(stack_top());
      }
    }

    void visit_swap_stack() {
      Value* top = stack_pop();
      Value* bottom = stack_pop();

      stack_push(top);
      stack_push(bottom);
    }

    void visit_dup_top() {
      stack_push(stack_top());
    }

    void visit_rotate(opcode count) {
      int diff = count / 2;

      for(int i = 0; i < diff; i++) {
        int offset = count - i - 1;
        Value* pos = stack_back_position(offset);
        Value* pos2 = stack_back_position(i);

        Value* val = b().CreateLoad(pos, "rotate");
        Value* val2 = b().CreateLoad(pos2, "rotate");

        b().CreateStore(val2, pos);
        b().CreateStore(val, pos2);
      }
    }

    void visit_move_down(opcode positions) {
      Value* val = stack_top();

      for(opcode i = 0; i < positions; i++) {
        int target = i;
        int current = target + 1;

        Value* tmp = stack_back(current);
        Value* pos = stack_back_position(target);

        b().CreateStore(tmp, pos);
      }

      b().CreateStore(val, stack_back_position(positions));
    }

    void check_fixnums(Value* left, Value* right, BasicBlock* if_true,
                       BasicBlock* if_false) {
      Value* mask = ConstantInt::get(IntPtrTy, TAG_FIXNUM_MASK);
      Value* tag  = ConstantInt::get(IntPtrTy, TAG_FIXNUM);

      Value* lint = cast_int(left);
      Value* rint = cast_int(right);
      Value* both =   b().CreateAnd(lint, rint, "both");
      Value* masked = b().CreateAnd(both, mask, "masked");

      Value* cmp = b().CreateICmpEQ(masked, tag, "are_fixnums");

      b().CreateCondBr(cmp, if_true, if_false);
    }

    void check_both_not_references(Value* left, Value* right, BasicBlock* if_true,
                            BasicBlock* if_false) {
      Value* mask = ConstantInt::get(IntPtrTy, TAG_REF_MASK);
      Value* zero = ConstantInt::get(IntPtrTy, TAG_REF);

      Value* lint = cast_int(left);
      lint = b().CreateAnd(lint, mask, "mask");
      Value* lcmp = b().CreateICmpNE(lint, zero, "check_mask");

      BasicBlock* right_check = new_block("ref_check");
      right_check->moveAfter(current_block());
      b().CreateCondBr(lcmp, right_check, if_false);

      set_block(right_check);

      Value* rint = cast_int(right);
      rint = b().CreateAnd(rint, mask, "mask");
      Value* rcmp = b().CreateICmpNE(rint, zero, "check_mask");

      b().CreateCondBr(rcmp, if_true, if_false);
    }

    void add_send_args(Signature& sig) {
      sig << VMTy;
      sig << CallFrameTy;
      sig << ObjType;
      sig << IntPtrTy;
      sig << ObjArrayTy;
    }

    Function* rbx_simple_send() {
      if(rbx_simple_send_) return rbx_simple_send_;

      Signature sig(ls_, ObjType);
      add_send_args(sig);

      rbx_simple_send_ = sig.function("rbx_simple_send");

      return rbx_simple_send_;
    }

    Function* rbx_simple_send_private() {
      if(rbx_simple_send_private_) return rbx_simple_send_private_;

      Signature sig(ls_, ObjType);
      add_send_args(sig);

      rbx_simple_send_private_ = sig.function("rbx_simple_send");

      return rbx_simple_send_private_;
    }

    Value* simple_send(Symbol* name, int args, bool priv=false) {
      sends_done_++;
      Function* func;
      if(priv) {
        func = rbx_simple_send_private();
      } else {
        func = rbx_simple_send();
      }

      Value* call_args[] = {
        vm_,
        call_frame_,
        constant(name),
        ConstantInt::get(IntPtrTy, args),
        stack_objects(args + 1)
      };

      return b().CreateCall(func, call_args, call_args+5, "simple_send");
    }

    void setup_out_args(int args) {
      b().CreateStore(stack_back(args), out_args_recv_);
      b().CreateStore(constant(Qnil), out_args_block_);
      b().CreateStore(ConstantInt::get(Type::Int32Ty, args),
                    out_args_total_);
      if(args > 0) {
        b().CreateStore(stack_objects(args), out_args_arguments_);
      }

      b().CreateStore(Constant::getNullValue(ptr_type("Array")),
                    out_args_array_);
    }

    void setup_out_args_with_block(int args) {
      b().CreateStore(stack_back(args + 1), out_args_recv_);
      b().CreateStore(stack_top(), out_args_block_);
      b().CreateStore(ConstantInt::get(Type::Int32Ty, args),
                    out_args_total_);
      if(args > 0) {
        b().CreateStore(stack_objects(args + 1), out_args_arguments_);
      }
      b().CreateStore(Constant::getNullValue(ptr_type("Array")),
                    out_args_array_);
    }

    Value* inline_cache_send(int args, InlineCache* cache) {
      sends_done_++;
      Value* cache_const = b().CreateIntToPtr(
          ConstantInt::get(IntPtrTy, reinterpret_cast<uintptr_t>(cache)),
          ptr_type("InlineCache"), "cast_to_ptr");

      Value* execute_pos_idx[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, 3),
      };

      Value* execute_pos = b().CreateGEP(cache_const,
          execute_pos_idx, execute_pos_idx+2, "execute_pos");

      Value* execute = b().CreateLoad(execute_pos, "execute");

      setup_out_args(args);

      Value* call_args[] = {
        vm_,
        cache_const,
        call_frame_,
        out_args_
      };

      return b().CreateCall(execute, call_args, call_args+4, "ic_send");
    }

    Value* block_send(InlineCache* cache, int args, bool priv=false) {
      sends_done_++;
      Value* cache_const = b().CreateIntToPtr(
          ConstantInt::get(IntPtrTy, reinterpret_cast<uintptr_t>(cache)),
          ptr_type("InlineCache"), "cast_to_ptr");

      Value* execute_pos_idx[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, 3),
      };

      Value* execute_pos = b().CreateGEP(cache_const,
          execute_pos_idx, execute_pos_idx+2, "execute_pos");

      Value* execute = b().CreateLoad(execute_pos, "execute");

      setup_out_args_with_block(args);

      Value* call_args[] = {
        vm_,
        cache_const,
        call_frame_,
        out_args_
      };

      return b().CreateCall(execute, call_args, call_args+4, "ic_send");
    }

    Value* splat_send(Symbol* name, int args, bool priv=false) {
      sends_done_++;
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << ObjType;
      sig << IntPtrTy;
      sig << ObjArrayTy;

      const char* func_name;
      if(priv) {
        func_name = "rbx_splat_send_private";
      } else {
        func_name = "rbx_splat_send";
      }

      Value* call_args[] = {
        vm_,
        call_frame_,
        constant(name),
        ConstantInt::get(IntPtrTy, args),
        stack_objects(args + 3),   // 3 == recv + block + splat
      };

      return sig.call(func_name, call_args, 5, "splat_send", b());
    }

    Value* super_send(Symbol* name, int args, bool splat=false) {
      Signature sig(ls_, ObjType);
      sig << VMTy;
      sig << CallFrameTy;
      sig << ObjType;
      sig << IntPtrTy;
      sig << ObjArrayTy;

      const char* func_name;
      int extra = 1;
      if(splat) {
        func_name = "rbx_super_splat_send";
        extra++;
      } else {
        func_name = "rbx_super_send";
      }

      Value* call_args[] = {
        vm_,
        call_frame_,
        constant(name),
        ConstantInt::get(IntPtrTy, args),
        stack_objects(args + extra),
      };

      return sig.call(func_name, call_args, 5, "super_send", b());
    }

    void visit_meta_send_op_equal(opcode name) {
      Value* recv = stack_back(1);
      Value* arg =  stack_top();

      BasicBlock* fast = new_block("fast");
      BasicBlock* dispatch = new_block("dispatch");
      BasicBlock* cont = new_block();

      check_both_not_references(recv, arg, fast, dispatch);

      set_block(dispatch);

      Value* called_value = simple_send(ls_->symbol("=="), 1);
      check_for_exception_then(called_value, cont);

      set_block(fast);

      Value* cmp = b().CreateICmpEQ(recv, arg, "imm_cmp");
      Value* imm_value = b().CreateSelect(cmp,
          constant(Qtrue), constant(Qfalse), "select_bool");

      b().CreateBr(cont);

      set_block(cont);

      PHINode* phi = b().CreatePHI(ObjType, "equal_value");
      phi->addIncoming(called_value, dispatch);
      phi->addIncoming(imm_value, fast);

      stack_remove(2);
      stack_push(phi);
    }

    void visit_meta_send_op_tequal(opcode name) {
      Value* recv = stack_back(1);
      Value* arg =  stack_top();

      BasicBlock* fast = new_block("fast");
      BasicBlock* dispatch = new_block("dispatch");
      BasicBlock* cont = new_block();

      check_fixnums(recv, arg, fast, dispatch);

      set_block(dispatch);

      Value* called_value = simple_send(ls_->symbol("==="), 1);
      check_for_exception_then(called_value, cont);

      set_block(fast);

      Value* cmp = b().CreateICmpEQ(recv, arg, "imm_cmp");
      Value* imm_value = b().CreateSelect(cmp,
          constant(Qtrue), constant(Qfalse), "select_bool");

      b().CreateBr(cont);

      set_block(cont);

      PHINode* phi = b().CreatePHI(ObjType, "equal_value");
      phi->addIncoming(called_value, dispatch);
      phi->addIncoming(imm_value, fast);

      stack_remove(2);
      stack_push(phi);
    }

    void visit_meta_send_op_lt(opcode name) {
      Value* recv = stack_back(1);
      Value* arg =  stack_top();

      BasicBlock* fast = new_block("fast");
      BasicBlock* dispatch = new_block("dispatch");
      BasicBlock* cont = new_block("cont");

      check_fixnums(recv, arg, fast, dispatch);

      set_block(dispatch);

      Value* called_value = simple_send(ls_->symbol("<"), 1);
      check_for_exception_then(called_value, cont);

      set_block(fast);

      Value* cmp = b().CreateICmpSLT(recv, arg, "imm_cmp");
      Value* imm_value = b().CreateSelect(cmp,
          constant(Qtrue), constant(Qfalse), "select_bool");

      b().CreateBr(cont);

      set_block(cont);

      PHINode* phi = b().CreatePHI(ObjType, "addition");
      phi->addIncoming(called_value, dispatch);
      phi->addIncoming(imm_value, fast);

      stack_remove(2);
      stack_push(phi);
    }

    void visit_meta_send_op_gt(opcode name) {
      Value* recv = stack_back(1);
      Value* arg =  stack_top();

      BasicBlock* fast = new_block("fast");
      BasicBlock* dispatch = new_block("dispatch");
      BasicBlock* cont = new_block("cont");

      check_fixnums(recv, arg, fast, dispatch);

      set_block(dispatch);

      Value* called_value = simple_send(ls_->symbol(">"), 1);
      check_for_exception_then(called_value, cont);

      set_block(fast);

      Value* cmp = b().CreateICmpSGT(recv, arg, "imm_cmp");
      Value* imm_value = b().CreateSelect(cmp,
          constant(Qtrue), constant(Qfalse), "select_bool");

      b().CreateBr(cont);

      set_block(cont);

      PHINode* phi = b().CreatePHI(ObjType, "compare");
      phi->addIncoming(called_value, dispatch);
      phi->addIncoming(imm_value, fast);

      stack_remove(2);
      stack_push(phi);
    }

    void visit_meta_send_op_plus(opcode name) {
      Value* recv = stack_back(1);
      Value* arg =  stack_top();

      BasicBlock* fast = new_block("fast");
      BasicBlock* dispatch = new_block("dispatch");
      BasicBlock* tagnow = new_block("tagnow");
      BasicBlock* cont = new_block("cont");

      check_fixnums(recv, arg, fast, dispatch);

      set_block(dispatch);

      Value* called_value = simple_send(ls_->symbol("+"), 1);
      check_for_exception_then(called_value, cont);

      set_block(fast);

      std::vector<const Type*> types;
      types.push_back(Int31Ty);
      types.push_back(Int31Ty);

      std::vector<const Type*> struct_types;
      struct_types.push_back(Int31Ty);
      struct_types.push_back(Type::Int1Ty);

      StructType* st = StructType::get(struct_types);

      FunctionType* ft = FunctionType::get(st, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("llvm.sadd.with.overflow.i31", ft));

      Value* recv_int = tag_strip(recv);
      Value* arg_int = tag_strip(arg);
      Value* call_args[] = { recv_int, arg_int };
      Value* res = b().CreateCall(func, call_args, call_args+2, "add.overflow");

      Value* sum = b().CreateExtractValue(res, 0, "sum");
      Value* dof = b().CreateExtractValue(res, 1, "did_overflow");

      b().CreateCondBr(dof, dispatch, tagnow);

      set_block(tagnow);

      Value* imm_value = fixnum_tag(sum);

      b().CreateBr(cont);

      set_block(cont);

      PHINode* phi = b().CreatePHI(ObjType, "addition");
      phi->addIncoming(called_value, dispatch);
      phi->addIncoming(imm_value, tagnow);

      stack_remove(2);
      stack_push(phi);
    }

    void visit_meta_send_op_minus(opcode name) {
      Value* recv = stack_back(1);
      Value* arg =  stack_top();

      BasicBlock* fast = new_block("fast");
      BasicBlock* dispatch = new_block("dispatch");
      BasicBlock* cont = new_block("cont");

      check_fixnums(recv, arg, fast, dispatch);

      set_block(dispatch);

      Value* called_value = simple_send(ls_->symbol("-"), 1);
      check_for_exception_then(called_value, cont);

      set_block(fast);

      std::vector<const Type*> types;
      types.push_back(Int31Ty);
      types.push_back(Int31Ty);

      std::vector<const Type*> struct_types;
      struct_types.push_back(Int31Ty);
      struct_types.push_back(Type::Int1Ty);

      StructType* st = StructType::get(struct_types);

      FunctionType* ft = FunctionType::get(st, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("llvm.ssub.with.overflow.i31", ft));

      Value* recv_int = tag_strip(recv);
      Value* arg_int = tag_strip(arg);
      Value* call_args[] = { recv_int, arg_int };
      Value* res = b().CreateCall(func, call_args, call_args+2, "sub.overflow");

      Value* sum = b().CreateExtractValue(res, 0, "sub");
      Value* dof = b().CreateExtractValue(res, 1, "did_overflow");

      BasicBlock* tagnow = new_block("tagnow");

      b().CreateCondBr(dof, dispatch, tagnow);

      set_block(tagnow);
      Value* imm_value = fixnum_tag(sum);

      b().CreateBr(cont);

      set_block(cont);

      PHINode* phi = b().CreatePHI(ObjType, "subtraction");
      phi->addIncoming(called_value, dispatch);
      phi->addIncoming(imm_value, tagnow);

      stack_remove(2);
      stack_push(phi);
    }

    Object* literal(opcode which) {
      return vmmethod()->original.get()->literals()->at(which);
    }

    Value* get_literal(opcode which) {
      Value* gep = b().CreateConstGEP2_32(call_frame_, 0, offset::cf_cm, "cm_pos");
      Value* cm =  b().CreateLoad(gep, "cm");

      gep = b().CreateConstGEP2_32(cm, 0, 13, "literals_pos");
      Value* lits = b().CreateLoad(gep, "literals");

      Value* idx2[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::tuple_field),
        ConstantInt::get(Type::Int32Ty, which)
      };

      gep = b().CreateGEP(lits, idx2, idx2+3, "literal_pos");
      return b().CreateLoad(gep, "literal");
    }

    void visit_push_literal(opcode which) {
      Object* lit = literal(which);
      if(Symbol* sym = try_as<Symbol>(lit)) {
        stack_push(constant(sym));
      } else {
        stack_push(get_literal(which));
      }
    }

    void visit_string_dup() {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_string_dup", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        stack_pop()
      };

      stack_push(b().CreateCall(func, call_args, call_args+3, "string_dup"));
    }

    void push_scope_local(Value* scope, opcode which) {
      Value* pos = b().CreateConstGEP2_32(scope, 0, offset::varscope_locals,
                                     "locals_pos");

      Value* table = b().CreateLoad(pos, "locals");

      Value* val_pos = b().CreateConstGEP1_32(table, which, "local_pos");

      stack_push(b().CreateLoad(val_pos, "local"));
    }

    void visit_push_local(opcode which) {
      Value* idx2[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
        ConstantInt::get(Type::Int32Ty, which)
      };

      Value* pos = b().CreateGEP(vars_, idx2, idx2+3, "local_pos");

      stack_push(b().CreateLoad(pos, "local"));
    }

    void set_scope_local(Value* scope, opcode which) {
      Value* pos = b().CreateConstGEP2_32(scope, 0, offset::varscope_locals,
                                     "locals_pos");

      Value* table = b().CreateLoad(pos, "locals");

      Value* val_pos = b().CreateConstGEP1_32(table, which, "local_pos");

      Value* val = stack_top();

      b().CreateStore(val, val_pos);

      write_barrier(scope, val);
    }

    void visit_set_local(opcode which) {
      Value* idx2[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
        ConstantInt::get(Type::Int32Ty, which)
      };

      Value* pos = b().CreateGEP(vars_, idx2, idx2+3, "local_pos");

      Value* val = stack_top();

      b().CreateStore(val, pos);
    }

    Value* get_self() {
      return b().CreateLoad(
          b().CreateConstGEP2_32(vars_, 0, offset::vars_self, "self_pos"));
    }

    Value* get_block() {
      return b().CreateLoad(
          b().CreateConstGEP2_32(vars_, 0, offset::vars_block, "self_pos"));
    }

    void visit_push_self() {
      stack_push(get_self());
    }

    void visit_allow_private() {
      allow_private_ = true;
    }

    void novisit_set_call_flags(opcode flag) {
      call_flags_ = flag;
    }

    void emit_uncommon() {
      Signature sig(ls_, "Object");

      sig << "VM";
      sig << "CallFrame";
      sig << "Arguments";
      sig << IntPtrTy;

      Value* sp = last_sp_as_int();

      Value* call_args[] = { vm_, call_frame_, args_, sp };

      Value* call = sig.call("rbx_continue_uncommon", call_args, 4, "", b());

      if(inline_return_) {
        return_value_->addIncoming(call, current_block());
        b().CreateBr(inline_return_);
      } else {
        b().CreateRet(call);
      }
    }

    void visit_send_stack(opcode which, opcode args) {
      InlineCache* cache = reinterpret_cast<InlineCache*>(which);
      BasicBlock* after = new_block("after_send");

      if(cache->method) {
        Inliner inl(*this, cache, args, after);
        // Uncommon doesn't yet know how to synthesize UnwindInfos, so
        // don't do uncommon if there are handlers.
        if(inl.consider() && exception_handlers_.size() == 0) {
          emit_uncommon();

          set_block(after);

          allow_private_ = false;
          return;
        }
      }

      reset_sp();

      Value* ret = inline_cache_send(args, cache);
      stack_remove(args + 1);
      check_for_exception(ret);
      stack_push(ret);

      b().CreateBr(after);
      set_block(after);

      allow_private_ = false;
    }

    void visit_send_method(opcode which) {
      InlineCache* cache = reinterpret_cast<InlineCache*>(which);
      BasicBlock* after = new_block("after_send");

      if(cache->method) {
        Inliner inl(*this, cache, 0, after);
        if(inl.consider() && exception_handlers_.size() == 0) {
          emit_uncommon();

          set_block(after);

          allow_private_ = false;
          return;
        }
      }

      reset_sp();

      Value* ret = inline_cache_send(0, cache);
      stack_remove(1);
      check_for_exception(ret);
      stack_push(ret);

      b().CreateBr(after);
      set_block(after);

      allow_private_ = false;
    }

    void visit_create_block(opcode which) {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(Type::Int32Ty);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_create_block", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, which)
      };

      stack_push(b().CreateCall(func, call_args, call_args+3, "create_block"));
    }

    void visit_send_stack_with_block(opcode which, opcode args) {
      InlineCache* cache = reinterpret_cast<InlineCache*>(which);
      Value* ret = block_send(cache, args, allow_private_);
      stack_remove(args + 2);
      check_for_return(ret);
      allow_private_ = false;
    }

    void visit_send_stack_with_splat(opcode which, opcode args) {
      InlineCache* cache = reinterpret_cast<InlineCache*>(which);
      Value* ret = splat_send(cache->name, args, allow_private_);
      stack_remove(args + 3);
      check_for_exception(ret);
      stack_push(ret);
      allow_private_ = false;
    }

    void visit_cast_array() {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_cast_array", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        stack_pop()
      };

      stack_push(b().CreateCall(func, call_args, call_args+3, "cast_array"));
    }

    void visit_push_block() {
      stack_push(get_block());
    }

    void visit_send_super_stack_with_block(opcode which, opcode args) {
      InlineCache* cache = reinterpret_cast<InlineCache*>(which);
      Value* ret = super_send(cache->name, args);
      stack_remove(args + 1);
      check_for_return(ret);
    }

    void visit_send_super_stack_with_splat(opcode which, opcode args) {
      InlineCache* cache = reinterpret_cast<InlineCache*>(which);
      Value* ret = super_send(cache->name, args, true);
      stack_remove(args + 2);
      check_for_exception(ret);
      stack_push(ret);
    }

    void visit_add_scope() {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_add_scope", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        stack_pop()
      };

      b().CreateCall(func, call_args, call_args+3);
    }

    Object* current_literal(opcode which) {
      return vmmethod()->original.get()->literals()->at(which);
    }

    void visit_push_const_fast(opcode name, opcode cache) {
      AccessManagedMemory memguard(ls_);

      BasicBlock* cont = 0;

      GlobalCacheEntry* entry = try_as<GlobalCacheEntry>(current_literal(cache));
      if(entry) {
        assert(entry->pin());

        Value* global_serial = b().CreateLoad(global_serial_pos, "global_serial");

        Value* current_serial_pos = b().CreateIntToPtr(
            ConstantInt::get(IntPtrTy, (intptr_t)entry->serial_location()),
            PointerType::getUnqual(IntPtrTy), "cast_to_intptr");

        Value* current_serial = b().CreateLoad(current_serial_pos, "serial");

        Value* cmp = b().CreateICmpEQ(global_serial, current_serial, "use_cache");

        BasicBlock* use_cache = new_block("use_cache");
        BasicBlock* use_call  = new_block("use_call");
        cont =      new_block("continue");

        b().CreateCondBr(cmp, use_cache, use_call);

        set_block(use_cache);

        Value* value_pos = b().CreateIntToPtr(
            ConstantInt::get(IntPtrTy, (intptr_t)entry->value_location()),
            PointerType::getUnqual(ObjType), "cast_to_objptr");

        stack_push(b().CreateLoad(value_pos, "cached_value"));

        b().CreateBr(cont);

        set_block(use_call);
      }

      reset_sp();

      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(ObjType);
      types.push_back(Type::Int32Ty);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_push_const_fast", ft));

      func->setOnlyReadsMemory(true);
      func->setDoesNotThrow(true);

      Value* call_args[] = {
        vm_,
        call_frame_,
        constant(as<Symbol>(literal(name))),
        ConstantInt::get(Type::Int32Ty, cache)
      };

      CallInst* ret = b().CreateCall(func, call_args, call_args+4,
                                       "push_const_fast");

      ret->setOnlyReadsMemory(true);
      ret->setDoesNotThrow(true);

      check_for_exception(ret);
      stack_push(ret);

      if(entry) {
        b().CreateBr(cont);

        set_block(cont);
      }
    }

    void visit_push_const(opcode name) {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_push_const", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        constant(as<Symbol>(literal(name)))
      };

      Value* ret = b().CreateCall(func, call_args, call_args+3, "push_const_fast");
      check_for_exception(ret);
      stack_push(ret);
    }

    void visit_set_const(opcode name) {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(ObjType);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_set_const", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        constant(as<Symbol>(literal(name))),
        stack_top()
      };

      b().CreateCall(func, call_args, call_args+4);
    }

    void visit_set_const_at(opcode name) {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(ObjType);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_set_const_at", ft));

      Value* val = stack_pop();
      Value* call_args[] = {
        vm_,
        constant(as<Symbol>(literal(name))),
        stack_top(),
        val
      };

      b().CreateCall(func, call_args, call_args+4);

      stack_push(val);
    }

    void visit_set_literal(opcode which) {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(Type::Int32Ty);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_set_literal", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, which),
        stack_top()
      };

      b().CreateCall(func, call_args, call_args+4);
    }

    void visit_push_variables() {
      Signature sig(ls_, ObjType);
      sig << "VM";
      sig << "CallFrame";

      Value* args[] = {
        vm_,
        call_frame_
      };

      stack_push(sig.call("rbx_promote_variables", args, 2, "promo_vars", b()));
    }

    void visit_push_scope() {
      Value* cm = b().CreateLoad(
          b().CreateConstGEP2_32(call_frame_, 0, offset::cf_cm, "cm_pos"),
          "cm");

      Value* gep = b().CreateConstGEP2_32(cm, 0, offset::cm_static_scope, "scope_pos");
      stack_push(b().CreateLoad(gep, "scope"));
    }

    void visit_cast_for_single_block_arg() {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(ptr_type("Arguments"));

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_cast_for_single_block_arg", ft));

      Value* call_args[] = {
        vm_,
        args_
      };

      stack_push(b().CreateCall(func, call_args, call_args+2, "cfsba"));
    }

    void visit_cast_for_multi_block_arg() {
      Signature sig(ls_, ObjType);
      sig << VMTy;
      sig << ptr_type("Arguments");

      Value* call_args[] = {
        vm_,
        args_
      };

      Value* val = sig.call("rbx_cast_for_multi_block_arg", call_args, 2,
                            "cfmba", b());
      stack_push(val);
    }

    void visit_cast_for_splat_block_arg() {
      Signature sig(ls_, ObjType);
      sig << VMTy;
      sig << ptr_type("Arguments");

      Value* call_args[] = {
        vm_,
        args_
      };

      Value* val = sig.call("rbx_cast_for_splat_block_arg", call_args, 2,
                            "cfmba", b());
      stack_push(val);
    }

    void visit_set_local_depth(opcode depth, opcode index) {
      if(depth == 0) {
        std::cout << "why is depth 0 here?\n";
        visit_set_local(index);
        return;
      } else if(depth == 1) {
        Value* idx[] = {
          ConstantInt::get(Type::Int32Ty, 0),
          ConstantInt::get(Type::Int32Ty, offset::vars_parent)
        };

        Value* gep = b().CreateGEP(vars_, idx, idx+2, "parent_pos");

        Value* parent = b().CreateLoad(gep, "scope.parent");
        set_scope_local(parent, index);
        return;
      }

      // Handle depth > 1

      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(ObjType);
      types.push_back(Type::Int32Ty);
      types.push_back(Type::Int32Ty);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_set_local_depth", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        stack_pop(),
        ConstantInt::get(Type::Int32Ty, depth),
        ConstantInt::get(Type::Int32Ty, index)
      };

      stack_push(b().CreateCall(func, call_args, call_args+5, "sld"));
    }

    void visit_push_local_depth(opcode depth, opcode index) {
      if(depth == 0) {
        std::cout << "why is depth 0 here?\n";
        visit_push_local(index);
        return;
      } else if(depth == 1) {
        Value* idx[] = {
          ConstantInt::get(Type::Int32Ty, 0),
          ConstantInt::get(Type::Int32Ty, offset::vars_parent)
        };

        Value* gep = b().CreateGEP(vars_, idx, idx+2, "parent_pos");

        Value* parent = b().CreateLoad(gep, "scope.parent");
        push_scope_local(parent, index);
        return;
      }

      // Handle depth > 1
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(Type::Int32Ty);
      types.push_back(Type::Int32Ty);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_push_local_depth", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, depth),
        ConstantInt::get(Type::Int32Ty, index)
      };

      stack_push(b().CreateCall(func, call_args, call_args+4, "pld"));
    }

    void visit_goto(opcode ip) {
      b().CreateBr(block_map_[ip].block);
      set_block(new_block("continue"));
    }

    void visit_goto_if_true(opcode ip) {
      Value* cond = stack_pop();
      Value* i = b().CreatePtrToInt(
          cond, IntPtrTy, "as_int");

      Value* anded = b().CreateAnd(i,
          ConstantInt::get(IntPtrTy, FALSE_MASK), "and");

      Value* cmp = b().CreateICmpNE(anded,
          ConstantInt::get(IntPtrTy, cFalse), "is_true");

      BasicBlock* cont = new_block("continue");
      b().CreateCondBr(cmp, block_map_[ip].block, cont);

      set_block(cont);
    }

    void visit_goto_if_false(opcode ip) {
      Value* cond = stack_pop();
      Value* i = b().CreatePtrToInt(
          cond, IntPtrTy, "as_int");

      Value* anded = b().CreateAnd(i,
          ConstantInt::get(IntPtrTy, FALSE_MASK), "and");

      Value* cmp = b().CreateICmpEQ(anded,
          ConstantInt::get(IntPtrTy, cFalse), "is_true");

      BasicBlock* cont = new_block("continue");
      b().CreateCondBr(cmp, block_map_[ip].block, cont);

      set_block(cont);
    }

    void visit_yield_stack(opcode count) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << Type::Int32Ty;
      sig << ObjArrayTy;

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, count),
        stack_objects(count)
      };

      Value* val = sig.call("rbx_yield_stack", call_args, 4, "ys", b());
      stack_remove(count);

      check_for_exception(val);
      stack_push(val);
    }

    void visit_yield_splat(opcode count) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << Type::Int32Ty;
      sig << ObjArrayTy;

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, count),
        stack_objects(count + 1)
      };

      Value* val = sig.call("rbx_yield_splat", call_args, 4, "ys", b());
      stack_remove(count + 1);

      check_for_exception(val);
      stack_push(val);
    }

    void visit_check_interrupts() {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_check_interrupts", ft));

      Value* call_args[] = {
        vm_,
        call_frame_
      };

      Value* ret = b().CreateCall(func, call_args, call_args+2, "ci");
      check_for_exception(ret);
    }

    void visit_check_serial(opcode index, opcode serial) {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(CallFrameTy);
      types.push_back(Type::Int32Ty);
      types.push_back(Type::Int32Ty);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_check_serial", ft));

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, index),
        ConstantInt::get(Type::Int32Ty, serial),
        stack_pop()
      };

      stack_push(b().CreateCall(func, call_args, call_args+5, "cs"));
    }

    void visit_push_my_offset(opcode i) {
      Value* idx[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::vars_self)
      };

      Value* pos = b().CreateGEP(vars_, idx, idx+2, "self_pos");

      Value* self = b().CreateLoad(pos, "self");

      assert(i % sizeof(Object*) == 0);

      Value* cst = b().CreateBitCast(
          self,
          PointerType::getUnqual(ObjType), "obj_array");

      Value* idx2[] = {
        ConstantInt::get(Type::Int32Ty, i / sizeof(Object*))
      };

      pos = b().CreateGEP(cst, idx2, idx2+1, "field_pos");

      stack_push(b().CreateLoad(pos, "field"));
    }

    void visit_setup_unwind(opcode where, opcode type) {
      BasicBlock* code;
      if(type == cRescue) {
        BasicBlock* orig = current_block();
        code = new_block("is_exception");
        set_block(code);

        std::vector<const Type*> types;
        types.push_back(VMTy);

        FunctionType* ft = FunctionType::get(Type::Int1Ty, types, false);
        Function* func = cast<Function>(
            module_->getOrInsertFunction("rbx_raising_exception", ft));

        Value* call_args[] = { vm_ };
        Value* isit = b().CreateCall(func, call_args, call_args+1, "rae");

        BasicBlock* next = 0;
        if(exception_handlers_.size() == 0) {
          next = bail_out_;
        } else {
          next = exception_handlers_.back();
        }

        b().CreateCondBr(isit, block_map_[where].block, next);

        set_block(orig);
      } else {
        code = block_map_[where].block;
      }

      exception_handlers_.push_back(code);
    }

    void visit_pop_unwind() {
      exception_handlers_.pop_back();
    }

    void visit_reraise() {
      if(exception_handlers_.size() > 0) {
        b().CreateBr(exception_handlers_.back());
      } else {
        b().CreateBr(bail_out_);
      }

      set_block(new_block("continue"));
    }

    void visit_raise_return() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << ObjType;

      Value* call_args[] = {
        vm_,
        call_frame_,
        stack_top()
      };

      sig.call("rbx_raise_return", call_args, 3, "raise_return", b());
      visit_reraise();
    }

    void visit_ensure_return() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << ObjType;

      Value* call_args[] = {
        vm_,
        call_frame_,
        stack_top()
      };

      sig.call("rbx_ensure_return", call_args, 3, "ensure_return", b());
      visit_reraise();
    }

    void visit_raise_break() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << ObjType;

      Value* call_args[] = {
        vm_,
        call_frame_,
        stack_top()
      };

      sig.call("rbx_raise_break", call_args, 3, "raise_break", b());
      visit_reraise();
    }

    void visit_push_exception() {
      std::vector<const Type*> types;

      types.push_back(VMTy);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_current_exception", ft));

      Value* call_args[] = { vm_ };

      stack_push(b().CreateCall(func, call_args, call_args+1, "ce"));
    }

    void visit_clear_exception() {
      std::vector<const Type*> types;

      types.push_back(VMTy);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_clear_exception", ft));

      Value* call_args[] = { vm_ };

      b().CreateCall(func, call_args, call_args+1);
    }

    void visit_pop_exception() {
      std::vector<const Type*> types;

      types.push_back(VMTy);
      types.push_back(ObjType);

      FunctionType* ft = FunctionType::get(ObjType, types, false);
      Function* func = cast<Function>(
          module_->getOrInsertFunction("rbx_pop_exception", ft));

      Value* call_args[] = { vm_, stack_pop() };

      b().CreateCall(func, call_args, call_args+2);
    }

    void visit_find_const(opcode which) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << Type::Int32Ty;
      sig << ObjType;

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, which),
        stack_pop()
      };

      Value* val = sig.call("rbx_find_const", call_args, 4, "constant", b());
      stack_push(val);
    }

    void visit_instance_of() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjType;
      sig << ObjType;

      Value* top = stack_pop();
      Value* call_args[] = {
        vm_,
        top,
        stack_pop()
      };

      Value* val = sig.call("rbx_instance_of", call_args, 3, "constant", b());
      stack_push(val);
    }

    void visit_kind_of() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjType;
      sig << ObjType;

      Value* top = stack_pop();
      Value* call_args[] = {
        vm_,
        top,
        stack_pop()
      };

      Value* val = sig.call("rbx_kind_of", call_args, 3, "constant", b());
      stack_push(val);
    }

    void visit_is_nil() {
      Value* cmp = b().CreateICmpEQ(stack_pop(),
          constant(Qnil), "is_nil");
      Value* imm_value = b().CreateSelect(cmp, constant(Qtrue),
          constant(Qfalse), "select_bool");
      stack_push(imm_value);
    }

    void visit_make_array(opcode count) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << Type::Int32Ty;
      sig << ObjArrayTy;

      Value* call_args[] = {
        vm_,
        ConstantInt::get(Type::Int32Ty, count),
        stack_objects(count)
      };

      Value* val = sig.call("rbx_make_array", call_args, 3, "constant", b());
      stack_remove(count);
      stack_push(val);
    }

    void visit_meta_send_call(opcode name, opcode count) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << CallFrameTy;
      sig << Type::Int32Ty;
      sig << ObjArrayTy;

      Value* call_args[] = {
        vm_,
        call_frame_,
        ConstantInt::get(Type::Int32Ty, count),
        stack_objects(count + 1)
      };

      Value* val = sig.call("rbx_meta_send_call", call_args, 4, "constant", b());
      stack_remove(count+1);
      check_for_exception(val);
      stack_push(val);
    }

    void visit_passed_arg(opcode count) {
      if(called_args_ >= 0) {
        if((int)count < called_args_) {
          stack_push(constant(Qtrue));
        } else {
          stack_push(constant(Qfalse));
        }
      } else {
        Signature sig(ls_, ObjType);

        sig << VMTy;
        sig << "Arguments";
        sig << Type::Int32Ty;

        Value* call_args[] = {
          vm_,
          args_,
          ConstantInt::get(Type::Int32Ty, count)
        };

        Value* val = sig.call("rbx_passed_arg", call_args, 3, "pa", b());
        stack_push(val);
      }
    }

    void visit_passed_blockarg(opcode count) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << "Arguments";
      sig << Type::Int32Ty;

      Value* call_args[] = {
        vm_,
        args_,
        ConstantInt::get(Type::Int32Ty, count)
      };

      Value* val = sig.call("rbx_passed_blockarg", call_args, 3, "pa", b());
      stack_push(val);
    }

    void visit_push_cpath_top() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << Type::Int32Ty;

      Value* call_args[] = {
        vm_,
        ConstantInt::get(Type::Int32Ty, 0)
      };

      Value* val = sig.call("rbx_push_system_object", call_args, 2, "so", b());
      stack_push(val);
    }

    void visit_push_ivar(opcode which) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjType;
      sig << ObjType;

      Value* self = get_self();

      Value* call_args[] = {
        vm_,
        self,
        constant(as<Symbol>(literal(which)))
      };

      Value* val = sig.call("rbx_push_ivar", call_args, 3, "ivar", b());
      // TODO: why would rbx_push_ivar raise an exception?
      // check_for_exception(val);
      stack_push(val);
    }

    void visit_set_ivar(opcode which) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjType;
      sig << ObjType;
      sig << ObjType;

      Value* self = get_self();

      Value* call_args[] = {
        vm_,
        self,
        constant(as<Symbol>(literal(which))),
        stack_top()
      };

      sig.call("rbx_set_ivar", call_args, 4, "ivar", b());
    }

    void visit_push_my_field(opcode which) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjType;
      sig << Type::Int32Ty;

      Value* self = get_self();

      Value* call_args[] = {
        vm_,
        self,
        ConstantInt::get(Type::Int32Ty, which)
      };

      Value* val = sig.call("rbx_push_my_field", call_args, 3, "field", b());
      check_for_exception(val);
      stack_push(val);
    }

    void visit_store_my_field(opcode which) {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjType;
      sig << Type::Int32Ty;
      sig << ObjType;

      Value* self = get_self();

      Value* call_args[] = {
        vm_,
        self,
        ConstantInt::get(Type::Int32Ty, which),
        stack_top()
      };

      sig.call("rbx_set_my_field", call_args, 4, "field", b());
    }

    void visit_shift_array() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjArrayTy;

      Value* call_args[] = {
        vm_,
        stack_back_position(0)
      };

      Value* val = sig.call("rbx_shift_array", call_args, 2, "field", b());
      stack_push(val);
    }

    void visit_string_append() {
      Signature sig(ls_, ObjType);

      sig << VMTy;
      sig << ObjType;
      sig << ObjType;

      Value* val = stack_pop();

      Value* call_args[] = {
        vm_,
        val,
        stack_pop()
      };

      Value* str = sig.call("rbx_string_append", call_args, 3, "string", b());
      stack_push(str);
    }
  };
}
