// Copyright 2015-2024 Carnegie Mellon University.  See LICENSE file for terms.

#include <AsmUnparser_compat.h>

#include "masm.hpp"
#include "util.hpp"
#include "misc.hpp"
#include "descriptors.hpp"
#include "cdg.hpp"

namespace pharos {

RoseLabelMap global_label_map{};

std::string masm_x86ValToLabel(uint64_t val, const RoseLabelMap *labels)
{
  if (!val || !labels)
    return "";

  RoseLabelMap::const_iterator li = labels->find(val);
  if (li==labels->end())
    return "";

  return li->second;
}

// Just a copy of the stock ROSE version?
std::string masm_x86TypeToPtrName(SgAsmType* ty) {
  if (NULL==ty) {
    GERROR << "masm_x86TypeToPtrName: null type" << LEND;
    return "BAD_TYPE";
  }

  if (SgAsmIntegerType *it = isSgAsmIntegerType(ty)) {
    switch (it->get_nBits()) {
     case 8: return "byte";
     case 16: return "word";
     case 32: return "dword";
     case 64: return "qword";
    }
  } else if (SgAsmFloatType *ft = isSgAsmFloatType(ty)) {
    switch (ft->get_nBits()) {
     case 32: return "float";
     case 64: return "double";
     case 80: return "ldouble";
    }
  } else if (ty == Rose::SageBuilderAsm::buildTypeVector(2, Rose::SageBuilderAsm::buildTypeU64())) {
    return "dqword";
  } else if (SgAsmVectorType *vt = isSgAsmVectorType(ty)) {
    return "V" + std::to_string(vt->get_nElmts()) + masm_x86TypeToPtrName(vt->get_elmtType());
  }
  ASSERT_not_reachable("unhandled type: " + ty->toString());
  return "BAD_TYPE";
}

namespace {

// This object looks for expressions of the form [reg1+reg2*I+C], where the expression could be
// put together in any order.  reg1 is the frame_pointer_reg, reg2 is the index_reg, I is the
// stride_integer, and C is the offset_integer.  ROSE at one point used the [reg1+reg2*I+C]
// order internally, but later changed to [reg1+C+reg2*I].  This code catches either order and
// can output the form in our preferred order.
struct X86IndirectAddress {
  SgAsmDirectRegisterExpression const * frame_pointer_reg = nullptr;
  SgAsmIntegerValueExpression const * offset_integer = nullptr;
  SgAsmDirectRegisterExpression const * index_reg = nullptr;
  SgAsmIntegerValueExpression const * stride_integer = nullptr;

  bool complete_ = false;

  explicit operator bool() {
    return complete_;
  }

  X86IndirectAddress(SgAsmExpression const * expr) {
    auto mre = isSgAsmMemoryReferenceExpression(expr);
    if (!mre) return;
    auto add1 = isSgAsmBinaryAdd(mre->get_address());
    if (!add1) return;
    SgAsmBinaryAdd const * add2 = nullptr;
    SgAsmExpression const * operand[3];
    if ((add2 = isSgAsmBinaryAdd(add1->get_lhs()))) {
      operand[0] = add2->get_lhs();
      operand[1] = add2->get_rhs();
      operand[2] = add1->get_rhs();
    } else if ((add2 = isSgAsmBinaryAdd(add1->get_rhs()))) {
      operand[0] = add1->get_lhs();
      operand[1] = add2->get_lhs();
      operand[2] = add2->get_rhs();
    }
    if (!add2) return;
    for (int i = 0; i < 3; ++i) {
      auto op = operand[i];
      if (auto reg = isSgAsmDirectRegisterExpression(op)) {
        if (frame_pointer_reg) return;
        frame_pointer_reg = reg;
      } else if (auto val = isSgAsmIntegerValueExpression(op)) {
        if (offset_integer) return;
        offset_integer = val;
      } else if (auto mul = isSgAsmBinaryMultiply(op)) {
        SgAsmDirectRegisterExpression const *r;
        SgAsmExpression const *other;
        if ((r = isSgAsmDirectRegisterExpression(mul->get_lhs()))) {
          other = mul->get_rhs();
        } else if ((r = isSgAsmDirectRegisterExpression(mul->get_rhs()))) {
          other = mul->get_lhs();
        } else {
          return;
        }
        if (auto v = isSgAsmIntegerValueExpression(other)) {
          if (index_reg) return;
          index_reg = r;
          stride_integer = v;
        } else {
          return;
        }
      }
    }
    complete_ = true;
  }

  std::string emit() const {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    os << '[';
    os << unparseX86Register(frame_pointer_reg->get_descriptor(),{});
    os << '+';
    os << unparseX86Register(index_reg->get_descriptor(),{});
    auto val = stride_integer->get_absoluteValue();
    if (val != 1) {
      os << '*' << val;
    }
    auto offset_bv = offset_integer->get_bitVector();
    bool neg = offset_bv.get(offset_bv.size() - 1);
    auto offset = offset_integer->get_signedValue();
    os << (neg ? '-' : '+');
    os << "0x" << (neg ? -offset : offset);
    os << ']';
    return os.str();
  }
};

}


std::string masm_unparseX86Expression(SgAsmExpression *expr,
                                      SgAsmX86Instruction *insn, bool leaMode,
                                      const RoseLabelMap *labels) {
  std::string result = "";
  if (expr == NULL) return "BOGUS:NULL";

  SgAsmExpression* lhs;
  SgAsmExpression* rhs;
  std::string lstr;
  std::string rstr;

  switch (expr->variantT()) {
   case V_SgAsmBinaryAdd:
    lhs = isSgAsmBinaryExpression(expr)->get_lhs();
    rhs = isSgAsmBinaryExpression(expr)->get_rhs();
    lstr = masm_unparseX86Expression(lhs, insn, false, labels);
    rstr = masm_unparseX86Expression(rhs, insn, false, labels);
    if (rstr[0] == '-') {
      result = lstr + rstr;
    }
    else {
      result = lstr + "+" + rstr;
    }
    break;
   case V_SgAsmBinarySubtract:
    lhs = isSgAsmBinaryExpression(expr)->get_lhs();
    rhs = isSgAsmBinaryExpression(expr)->get_rhs();
    lstr = masm_unparseX86Expression(lhs, insn, false, labels);
    rstr = masm_unparseX86Expression(rhs, insn, false, labels);
    result = lstr + "-" + rstr;
    break;
   case V_SgAsmBinaryMultiply:
    lhs = isSgAsmBinaryExpression(expr)->get_lhs();
    rhs = isSgAsmBinaryExpression(expr)->get_rhs();
    lstr = masm_unparseX86Expression(lhs, insn, false, labels);
    rstr = masm_unparseX86Expression(rhs, insn, false, labels);
    result = lstr + "*" + rstr;
    break;
   case V_SgAsmMemoryReferenceExpression: {
     X86IndirectAddress ia(expr);
     if (ia) {
       // If address is of the form [reg1+reg2*I+C], handle that separately.  See the comment
       // for X86IndirectAddress.
       return ia.emit();
     } else {
       SgAsmMemoryReferenceExpression* mr = isSgAsmMemoryReferenceExpression(expr);
       if (!leaMode) {
         // The MASM and NASM assemblers are quite intelligent about
         // determining the memory size of the operation.  We only need
         // to explictly display the size if it's ambiguous.  For now,
         // we're just going to say that it's never ambiguous.  BUG!!!
         bool ambiguous = false;

         if (ambiguous) {
           result += masm_x86TypeToPtrName(mr->get_type()) + " ptr ";
         }

         std::string segment_override = "";
         // We really should be checking for a segment override prefix here, but the
         // method is private, and I expect overrides to be very rare anyway.  BUG!!!
         SgAsmExpression* segexpr = mr->get_segment();
         if (segexpr != NULL) {
           // The fs override is common enough that we should always print it.
           std::string segreg = masm_unparseX86Expression(segexpr, insn, false, NULL);
           if (segreg == "fs") {
             result += segreg + ":";
           }
         }
       }
       result += "[" + masm_unparseX86Expression(mr->get_address(), insn, false, labels) + "]";
     }
     break;
   }
   case V_SgAsmDirectRegisterExpression: {
     SgAsmDirectRegisterExpression* rr = isSgAsmDirectRegisterExpression(expr);
     result = unparseX86Register(rr->get_descriptor(),{});
     break;
   }
    // Cory says: This case demonstrates how crusty masm.cpp has gotten.  The new floating
    // point case was missing entirely, and when copied into our code, it wouldn't compile
    // because the entire interface to unparsex86Expression has changed.  We should try to
    // report our changes to a current version of unparseAsm.C.  BUG!!! BUG!!! BUG!!!
   case V_SgAsmIndirectRegisterExpression: {
     //SgAsmInstruction *tinsn = Rose::SageInterface::getEnclosingNode<SgAsmInstruction>(expr);
     SgAsmIndirectRegisterExpression* rr = isSgAsmIndirectRegisterExpression(expr);
     //result = unparseX86Register(insn, rr->get_descriptor(), registers);
     //if (!result.empty() && '0'==result[result.size()-1])
     //  result = result.substr(0, result.size()-1);
     result += "(" + std::to_string(rr->get_index()) + ")";
     break;
   }
   case V_SgAsmIntegerValueExpression: {
     char buf[64];
     SgAsmIntegerValueExpression* int_expr = isSgAsmIntegerValueExpression(expr);
     uint64_t v = SageInterface::getAsmConstant(int_expr);
     // Cory says there's probably a better way to do this since I haven't looked at this code
     // in forever.  But the goal right now is to build under the new version of ROSE.
     size_t bits = int_expr->get_significantBits();
     if (bits == 8) {
       if ((v & 0x80) && (v & 0x7f))
         sprintf(buf, "-%#" PRIx64, (~v+1) & 0xff);
       else
         sprintf(buf, "%#" PRIx64, v);

       result = buf;
     }
     else if (bits == 16) {
       if ((v & 0x8000) && (v & 0x7fff))
         sprintf(buf, "-%#" PRIx64, (~v+1) & 0xffff);
       else
         sprintf(buf, "%#" PRIx64, v);
       result = buf;
     }
     else if (bits == 32) {
       std::string label = masm_x86ValToLabel(v, labels);
       if (!label.empty()) {
         sprintf(buf, "%s", label.c_str());
       } else if ((v & 0x80000000) && (v & 0x7fffffff)) {
         sprintf(buf, "-%#" PRIx64, (~v+1) & 0xffffffff);
       }
       else {
         sprintf(buf, "%#" PRIx64, v);
       }
       result = buf;
     }
     else if (bits == 64) {
       std::string label = masm_x86ValToLabel(v, labels);
       if (!label.empty()) {
         sprintf(buf, "%s", label.c_str());
       } else if ((v & ((uint64_t)1<<63)) && (v & (((uint64_t)1<<63)-1))) {
         sprintf(buf, "-%#" PRIx64, (~v+1));
       }
       else {
         sprintf(buf, "%#" PRIx64, v);
       }
       result = buf;
     }
     break;
   }
   default: {
     GFATAL << "Unhandled expression kind " << expr->class_name() << LEND;
     ROSE_ASSERT (false);
   }
  }

#if 0
  if (expr->get_replacement() != "") {
    result += " <" + expr->get_replacement() + ">";
  }
  if (expr->get_bit_size()>0) {
    result += " <@" + std::to_string(expr->get_bit_offset()) +
              "+" + std::to_string(expr->get_bit_size()) + ">";
  }
#endif
  return result;
}

/** Returns a string containing the specified operand. */
std::string masm_unparseX86Expression(SgAsmExpression *expr, const RoseLabelMap *labels) {
  /* Find the instruction with which this expression is associated. */
  SgAsmX86Instruction* insn = NULL;
  for (SgNode *node=expr; !insn && node; node=node->get_parent()) {
    insn = isSgAsmX86Instruction(node);
    if (insn) {
      return masm_unparseX86Expression(expr, insn, insn->get_kind()==x86_lea, labels);
    }
  }
  return "?";
}

// Use our really old and hacky X86 code, but fall back to the standard ROSE routines for all
// other architectures.
std::string masm_unparseExpression(
  const SgAsmInstruction *insn,
  const SgAsmExpression *expr,
  RegisterDictionaryPtrArg rdict,
  const RoseLabelMap *labels)
{
  // Const casts are to hide sillyness in the ROSE AST API that incorrectly lacks const.
  if (isSgAsmX86Instruction(insn)) {
    return masm_unparseX86Expression(const_cast<SgAsmExpression *>(expr), labels);
  }
  else {
    return unparseExpression(const_cast<SgAsmExpression *>(expr), labels, rdict);
  }
}

std::string debug_opcode_bytes(const SgUnsignedCharList& data, const unsigned int max_bytes)
{
  char buffer[8];
  std::string result = "";

  size_t n = data.size();
  if (n > max_bytes) n = max_bytes;
  for (size_t i = 0; i < n; i++) {
    sprintf(buffer,"%02X", data[i]);
    result += buffer;
  }
  if (data.size() > max_bytes) result += "+";
  return result;
}

std::string debug_function(const FunctionDescriptor* fd, const unsigned int max_bytes,
                           const bool basic_block_lines, const bool show_reasons,
                           const RoseLabelMap *labels)
{
  std::string result = "";
  CFG cfg = fd->get_rose_cfg();

  for (auto vertex : fd->get_vertices_in_flow_order(cfg)) {
    SgNode *n = get(boost::vertex_name, cfg, vertex);
    SgAsmBlock *blk = isSgAsmBlock(n);
    assert(blk != NULL);
    if (show_reasons) {
      result += "; block reason: " + blk->reasonString(false) + "\n";
    }
#if 1
    if (isSgAsmStaticData(blk)) {
      result += "; hey, this block is static data!\n";
    }
#endif
    for (size_t j = 0; j < blk->get_statementList().size(); ++j) {
      SgAsmStatement *stmt = blk->get_statementList()[j];
      SgAsmInstruction *inst = isSgAsmInstruction(stmt);
      assert(inst != NULL);
      result += debug_instruction(inst, max_bytes, labels) + '\n';
    }
    if (basic_block_lines) result += "\n";
  }
  return result;
}

std::string debug_instruction(const SgAsmInstruction *inst, const unsigned int max_bytes,
                              const RoseLabelMap *labels)
{
  char buffer[512];
  std::string opbytes = "";

  if (inst == NULL) return "NULL!";
  if (!isSgAsmX86Instruction(inst)) {
    SgAsmInstruction *ncinsn = const_cast<SgAsmInstruction *>(inst);
    if (max_bytes > 0) {
      opbytes = " ; BYTES: " + debug_opcode_bytes(inst->get_rawBytes(), max_bytes);
    }
    return addr_str(inst->get_address()) + " " + unparseInstruction(ncinsn) + opbytes;
  }

  SgAsmOperandList *oplist = inst->get_operandList();
  SgAsmExpressionPtrList& elist = oplist->get_operands();
  std::string opstr = "";
  for (SgAsmExpressionPtrList::iterator exp = elist.begin(); exp != elist.end(); ++exp) {
    opstr.append(masm_unparseX86Expression(*exp, labels).c_str());
    if(exp != elist.end() -1)
      opstr.append(", ");
  }

  if (max_bytes > 0) {
    opbytes = " ; BYTES: " + debug_opcode_bytes(inst->get_rawBytes(), max_bytes);
  }
  snprintf(buffer, sizeof(buffer), "%0" PRIX64 ": %-9s %s", inst->get_address(),
           inst->get_mnemonic().c_str(), opstr.c_str());

  return buffer + opbytes;
}

} // namespace pharos

/* Local Variables:   */
/* mode: c++          */
/* fill-column:    95 */
/* comment-column: 0  */
/* End:               */
