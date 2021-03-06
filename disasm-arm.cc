#include <capstone/capstone.h>

#include "disasm-arm.h"
#include "log.h"


static int
is_cs_nop_ins(cs_insn *ins)
{
  switch(ins->id) {
  case ARM_INS_NOP:
    return 1;
  default:
    return 0;
  }
}


static int
is_cs_trap_ins(cs_insn *ins)
{
  switch(ins->id) {
  /* XXX: todo */
  default:
    return 0;
  }
}


static int
is_cs_call_ins(cs_insn *ins)
{
  switch(ins->id) {
  case ARM_INS_BL:
  case ARM_INS_BLX:
    return 1;
  default:
    return 0;
  }
}


static int
is_cs_ret_ins(cs_insn *ins)
{
  size_t i;

  /* bx lr */
  if(ins->id == ARM_INS_BX
     && ins->detail->arm.op_count == 1
     && ins->detail->arm.operands[0].type == ARM_OP_REG
     && ins->detail->arm.operands[0].reg == ARM_REG_LR) {
    return 1;
  }

  /* ldmfd sp!, {..., pc} */
  if(ins->id == ARM_INS_POP) {
    for(i = 0; i < ins->detail->arm.op_count; i++) {
      if(ins->detail->arm.operands[i].type == ARM_OP_REG &&
         ins->detail->arm.operands[i].reg == ARM_REG_PC) {
        return 1;
      }
    }
  }

  /* mov pc, lr */
  if(ins->id == ARM_INS_MOV
     && ins->detail->arm.operands[0].type == ARM_OP_REG
     && ins->detail->arm.operands[0].reg == ARM_REG_PC
     && ins->detail->arm.operands[1].type == ARM_OP_REG
     && ins->detail->arm.operands[1].reg == ARM_REG_LR) {
    return 1;
  }

  return 0;
}


static int
is_cs_unconditional_jmp_ins(cs_insn *ins)
{
  /* b rN */
  if(ins->id == ARM_INS_B
     && ins->detail->arm.cc == ARM_CC_AL) {
    return 1;
  }

  /* mov pc, rN */
  if(ins->id == ARM_INS_MOV
     && ins->detail->arm.operands[0].type == ARM_OP_REG
     && ins->detail->arm.operands[0].reg == ARM_REG_PC
     && ins->detail->arm.operands[1].type == ARM_OP_REG
     && ins->detail->arm.operands[1].reg != ARM_REG_LR) {
    return 1;
  }

  /* ldrls pc, {...} */
  if(ins->id == ARM_INS_LDR
     && ins->detail->arm.operands[0].type == ARM_OP_REG
     && ins->detail->arm.operands[0].reg == ARM_REG_PC) {
    return 1;
  }

  return 0;
}


static int
is_cs_conditional_cflow_ins(cs_insn *ins)
{
  switch(ins->id) {
  case ARM_INS_B:
  case ARM_INS_BL:
  case ARM_INS_BLX:
    if (ins->detail->arm.cc != ARM_CC_AL) {
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}


static int
is_cs_cflow_ins(cs_insn *ins)
{
  /* XXX: Capstone does not provide information for all generic groups
   * for arm instructions, unlike x86, so we have to do it manually.
   * Once this is implemented, it will suffice to check for the following groups:
   * CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_IRET */

  if(is_cs_unconditional_jmp_ins(ins) ||
     is_cs_conditional_cflow_ins(ins) ||
     is_cs_call_ins(ins) ||
     is_cs_ret_ins(ins)) {
    return 1;
  }

  return 0;
}


static int
is_cs_indirect_ins(cs_insn *ins)
{
  /* mov pc, rN */
  if(ins->id == ARM_INS_MOV
     && ins->detail->arm.operands[0].type == ARM_OP_REG
     && ins->detail->arm.operands[0].reg == ARM_REG_PC
     && ins->detail->arm.operands[1].type == ARM_OP_REG
     && ins->detail->arm.operands[1].reg != ARM_REG_LR) {
    return 1;
  }

  /* ldrls pc, {...} */
  if(ins->id == ARM_INS_LDR
     && ins->detail->arm.operands[0].type == ARM_OP_REG
     && ins->detail->arm.operands[0].reg == ARM_REG_PC) {
    return 1;
  }

  switch(ins->id) {
  case ARM_INS_BX:
  case ARM_INS_BLX:
  case ARM_INS_BXJ:
    if(ins->detail->arm.operands[0].type == ARM_OP_REG &&
       ins->detail->arm.operands[0].reg == ARM_REG_PC) {
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}


static int
is_cs_privileged_ins(cs_insn *ins)
{
  switch(ins->id) {
  /* XXX: todo */
  default:
    return 0;
  }
}


static uint8_t
cs_to_nucleus_op_type(arm_op_type op)
{
  switch(op) {
  case ARM_OP_REG:
    return OP_TYPE_REG;
  case ARM_OP_IMM:
    return OP_TYPE_IMM;
  case ARM_OP_MEM:
    return OP_TYPE_MEM;
  case ARM_OP_FP:
    return OP_TYPE_FP;
  case ARM_OP_INVALID:
  default:
    return OP_TYPE_NONE;
  }
}


int
nucleus_disasm_bb_arm(Binary *bin, DisasmSection *dis, BB *bb)
{
  int init, ret, jmp, indir, cflow, cond, call, nop, only_nop, priv, trap, ndisassembled;
  csh cs_dis;
  cs_mode cs_mode_flags;
  cs_insn *cs_ins;
  cs_arm_op *cs_op;
  const uint8_t *pc;
  uint64_t pc_addr, offset;
  size_t i, j, n;
  Instruction *ins;
  ARMOperand *op;

  init   = 0;
  cs_ins = NULL;

  switch(bin->bits) {
  case 32:
    cs_mode_flags = (cs_mode)(CS_MODE_ARM);
    break;
  default:
    print_err("unsupported bit width %u for architecture %s", bin->bits, bin->arch_str.c_str());
    goto fail;
  }

  if(cs_open(CS_ARCH_ARM, cs_mode_flags, &cs_dis) != CS_ERR_OK) {
    print_err("failed to initialize libcapstone");
    goto fail;
  }
  init = 1;
  cs_option(cs_dis, CS_OPT_DETAIL, CS_OPT_ON);

  cs_ins = cs_malloc(cs_dis);
  if(!cs_ins) {
    print_err("out of memory");
    goto fail;
  }

  offset = bb->start - dis->section->vma;
  if((bb->start < dis->section->vma) || (offset >= dis->section->size)) {
    print_err("basic block address points outside of section '%s'", dis->section->name.c_str());
    goto fail;
  }

  pc = dis->section->bytes + offset;
  n = dis->section->size - offset;
  pc_addr = bb->start;
  bb->end = bb->start;
  bb->section = dis->section;
  ndisassembled = 0;
  only_nop = 0;
  while(cs_disasm_iter(cs_dis, &pc, &n, &pc_addr, cs_ins)) {
    if(cs_ins->id == ARM_INS_INVALID) {
      bb->invalid = 1;
      bb->end += 1;
      break;
    }
    if(!cs_ins->size) {
      break;
    }

    trap  = is_cs_trap_ins(cs_ins);
    nop   = is_cs_nop_ins(cs_ins);
    ret   = is_cs_ret_ins(cs_ins);
    jmp   = is_cs_unconditional_jmp_ins(cs_ins) || is_cs_conditional_cflow_ins(cs_ins);
    cond  = is_cs_conditional_cflow_ins(cs_ins);
    cflow = is_cs_cflow_ins(cs_ins);
    call  = is_cs_call_ins(cs_ins);
    priv  = is_cs_privileged_ins(cs_ins);
    indir = is_cs_indirect_ins(cs_ins);

    if(!ndisassembled && nop) only_nop = 1; /* group nop instructions together */
    if(!only_nop && nop) break;
    if(only_nop && !nop) break;

    ndisassembled++;

    bb->end += cs_ins->size;
    bb->insns.push_back(Instruction());
    if(priv) {
      bb->privileged = true;
    }
    if(nop) {
      bb->padding = true;
    }
    if(trap) {
      bb->trap = true;
    }

    ins = &bb->insns.back();
    ins->id         = cs_ins->id;
    ins->start      = cs_ins->address;
    ins->size       = cs_ins->size;
    ins->mnem       = std::string(cs_ins->mnemonic);
    ins->op_str     = std::string(cs_ins->op_str);
    ins->privileged = priv;
    ins->trap       = trap;
    if(nop)   ins->flags |= INS_FLAG_NOP;
    if(ret)   ins->flags |= INS_FLAG_RET;
    if(jmp)   ins->flags |= INS_FLAG_JMP;
    if(cond)  ins->flags |= INS_FLAG_COND;
    if(cflow) ins->flags |= INS_FLAG_CFLOW;
    if(call)  ins->flags |= INS_FLAG_CALL;
    if(indir) ins->flags |= INS_FLAG_INDIRECT;

    for(i = 0; i < cs_ins->detail->arm.op_count; i++) {
      cs_op = &cs_ins->detail->arm.operands[i];
      ins->operands.push_back(Operand());
      op = dynamic_cast<ARMOperand*>(&ins->operands.back());
      op->type = cs_to_nucleus_op_type(cs_op->type);
      if(op->type == OP_TYPE_IMM) {
        op->value.imm = cs_op->imm;
      } else if(op->type == OP_TYPE_REG) {
        op->value.reg = (arm_reg)cs_op->reg;
      } else if(op->type == OP_TYPE_FP) {
        op->value.fp = cs_op->fp;
      } else if(op->type == OP_TYPE_MEM) {
        op->value.mem.base    = cs_op->mem.base;
        op->value.mem.index   = cs_op->mem.index;
        op->value.mem.scale   = cs_op->mem.scale;
        op->value.mem.disp    = cs_op->mem.disp;
        if(cflow) ins->flags |= INS_FLAG_INDIRECT;
      }
    }

    if(cflow) {
      for(j = 0; j < cs_ins->detail->arm.op_count; j++) {
        cs_op = &cs_ins->detail->arm.operands[j];
        if(cs_op->type == ARM_OP_IMM) {
          ins->target = cs_op->imm;
        }
      }
    }

    if(cflow) {
      /* end of basic block */
      break;
    }
  }

  if(!ndisassembled) {
    bb->invalid = 1;
    bb->end += 1; /* ensure forward progress */
  }

  ret = ndisassembled;
  goto cleanup;

  fail:
  ret = -1;

  cleanup:
  if(cs_ins) {
    cs_free(cs_ins, 1);
  }
  if(init) {
    cs_close(&cs_dis);
  }
  return ret;
}
