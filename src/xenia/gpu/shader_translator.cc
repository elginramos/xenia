/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/shader_translator.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <set>
#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/gpu_flags.h"

namespace xe {
namespace gpu {

using namespace ucode;

// The Xbox 360 GPU is effectively an Adreno A200:
// https://github.com/freedreno/freedreno/wiki/A2XX-Shader-Instruction-Set-Architecture
//
// A lot of this information is derived from the freedreno drivers, AMD's
// documentation, publicly available Xbox presentations (from GDC/etc), and
// other reverse engineering.
//
// Naming has been matched as closely as possible to the real thing by using the
// publicly available XNA Game Studio shader assembler.
// You can find a tool for exploring this under tools/shader-playground/,
// allowing interative assembling/disassembling of shader code.
//
// Though the 360's GPU is similar to the Adreno r200, the microcode format is
// slightly different. Though this is a great guide it cannot be assumed it
// matches the 360 in all areas:
// https://github.com/freedreno/freedreno/blob/master/util/disasm-a2xx.c
//
// Lots of naming comes from the disassembly spit out by the XNA GS compiler
// and dumps of d3dcompiler and games: https://pastebin.com/i4kAv7bB

void Shader::AnalyzeUcode(StringBuffer& ucode_disasm_buffer) {
  if (is_ucode_analyzed_) {
    return;
  }

  // Control flow instructions come paired in blocks of 3 dwords and all are
  // listed at the top of the ucode.
  // Each control flow instruction is executed sequentially until the final
  // ending instruction.
  // Gather the upper bound of the control flow instructions, and label
  // addresses, which are needed for disassembly.
  cf_pair_index_bound_ = uint32_t(ucode_data_.size() / 3);
  for (uint32_t i = 0; i < cf_pair_index_bound_; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      // Guess how long the control flow program is by scanning for the first
      // kExec-ish and instruction and using its address as the upper bound.
      // This is what freedreno does.
      const ControlFlowInstruction& cf = cf_ab[j];
      if (IsControlFlowOpcodeExec(cf.opcode())) {
        cf_pair_index_bound_ =
            std::min(cf_pair_index_bound_, cf.exec.address());
      }
      switch (cf.opcode()) {
        case ControlFlowOpcode::kCondCall:
          label_addresses_.insert(cf.cond_call.address());
          break;
        case ControlFlowOpcode::kCondJmp:
          label_addresses_.insert(cf.cond_jmp.address());
          break;
        case ControlFlowOpcode::kLoopStart:
          label_addresses_.insert(cf.loop_start.address());
          break;
        case ControlFlowOpcode::kLoopEnd:
          label_addresses_.insert(cf.loop_end.address());
          break;
        default:
          break;
      }
    }
  }

  // Disassemble and gather information.
  ucode_disasm_buffer.Reset();
  VertexFetchInstruction previous_vfetch_full;
  std::memset(&previous_vfetch_full, 0, sizeof(previous_vfetch_full));
  uint32_t unique_texture_bindings = 0;
  uint32_t memexport_alloc_count = 0;
  uint32_t memexport_eA_written = 0;
  for (uint32_t i = 0; i < cf_pair_index_bound_; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      uint32_t cf_index = i * 2 + j;
      if (label_addresses_.find(cf_index) != label_addresses_.end()) {
        ucode_disasm_buffer.AppendFormat("                label L{}\n",
                                         cf_index);
      }
      ucode_disasm_buffer.AppendFormat("/* {:4d}.{} */ ", i, j);

      const ControlFlowInstruction& cf = cf_ab[j];
      uint32_t bool_constant_index = UINT32_MAX;
      switch (cf.opcode()) {
        case ControlFlowOpcode::kNop:
          ucode_disasm_buffer.Append("      cnop\n");
          break;
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd: {
          ParsedExecInstruction instr;
          ParseControlFlowExec(cf.exec, cf_index, instr);
          GatherExecInformation(instr, previous_vfetch_full,
                                unique_texture_bindings, memexport_alloc_count,
                                memexport_eA_written, ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd: {
          bool_constant_index = cf.cond_exec.bool_address();
          ParsedExecInstruction instr;
          ParseControlFlowCondExec(cf.cond_exec, cf_index, instr);
          GatherExecInformation(instr, previous_vfetch_full,
                                unique_texture_bindings, memexport_alloc_count,
                                memexport_eA_written, ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd: {
          ParsedExecInstruction instr;
          ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index, instr);
          GatherExecInformation(instr, previous_vfetch_full,
                                unique_texture_bindings, memexport_alloc_count,
                                memexport_eA_written, ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kLoopStart: {
          ParsedLoopStartInstruction instr;
          ParseControlFlowLoopStart(cf.loop_start, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          constant_register_map_.loop_bitmap |= uint32_t(1)
                                                << instr.loop_constant_index;
        } break;
        case ControlFlowOpcode::kLoopEnd: {
          ParsedLoopEndInstruction instr;
          ParseControlFlowLoopEnd(cf.loop_end, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          constant_register_map_.loop_bitmap |= uint32_t(1)
                                                << instr.loop_constant_index;
        } break;
        case ControlFlowOpcode::kCondCall: {
          ParsedCallInstruction instr;
          ParseControlFlowCondCall(cf.cond_call, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          if (instr.type == ParsedCallInstruction::Type::kConditional) {
            bool_constant_index = instr.bool_constant_index;
          }
        } break;
        case ControlFlowOpcode::kReturn: {
          ParsedReturnInstruction instr;
          ParseControlFlowReturn(cf.ret, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kCondJmp: {
          ParsedJumpInstruction instr;
          ParseControlFlowCondJmp(cf.cond_jmp, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          if (instr.type == ParsedJumpInstruction::Type::kConditional) {
            bool_constant_index = instr.bool_constant_index;
          }
        } break;
        case ControlFlowOpcode::kAlloc: {
          ParsedAllocInstruction instr;
          ParseControlFlowAlloc(cf.alloc, cf_index,
                                type() == xenos::ShaderType::kVertex, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          if (instr.type == AllocType::kMemory) {
            ++memexport_alloc_count;
          }
        } break;
        case ControlFlowOpcode::kMarkVsFetchDone:
          break;
        default:
          assert_unhandled_case(cf.opcode);
          break;
      }
      if (bool_constant_index != UINT32_MAX) {
        constant_register_map_.bool_bitmap[bool_constant_index / 32] |=
            uint32_t(1) << (bool_constant_index % 32);
      }
      // TODO(benvanik): break if (DoesControlFlowOpcodeEndShader(cf.opcode()))?
    }
  }
  ucode_disassembly_ = ucode_disasm_buffer.to_string();

  if (constant_register_map_.float_dynamic_addressing) {
    // All potentially can be referenced.
    constant_register_map_.float_count = 256;
    memset(constant_register_map_.float_bitmap, UINT8_MAX,
           sizeof(constant_register_map_.float_bitmap));
  } else {
    constant_register_map_.float_count = 0;
    for (int i = 0; i < 4; ++i) {
      // Each bit indicates a vec4 (4 floats).
      constant_register_map_.float_count +=
          xe::bit_count(constant_register_map_.float_bitmap[i]);
    }
  }

  // Cleanup invalid/unneeded memexport allocs.
  for (uint32_t i = 0; i < kMaxMemExports; ++i) {
    if (!(memexport_eA_written & (uint32_t(1) << i))) {
      memexport_eM_written_[i] = 0;
    } else if (!memexport_eM_written_[i]) {
      memexport_eA_written &= ~(uint32_t(1) << i);
    }
  }
  if (memexport_eA_written == 0) {
    memexport_stream_constants_.clear();
  }

  is_ucode_analyzed_ = true;

  // An empty shader can be created internally by shader translators as a dummy,
  // don't dump it.
  if (!cvars::dump_shaders.empty() && !ucode_data().empty()) {
    DumpUcode(cvars::dump_shaders);
  }
}

void Shader::GatherExecInformation(
    const ParsedExecInstruction& instr,
    ucode::VertexFetchInstruction& previous_vfetch_full,
    uint32_t& unique_texture_bindings, uint32_t memexport_alloc_current_count,
    uint32_t& memexport_eA_written, StringBuffer& ucode_disasm_buffer) {
  instr.Disassemble(&ucode_disasm_buffer);
  uint32_t sequence = instr.sequence;
  for (uint32_t instr_offset = instr.instruction_address;
       instr_offset < instr.instruction_address + instr.instruction_count;
       ++instr_offset, sequence >>= 2) {
    ucode_disasm_buffer.AppendFormat("/* {:4d}   */ ", instr_offset);
    if (sequence & 0b10) {
      ucode_disasm_buffer.Append("         serialize\n             ");
    }
    if (sequence & 0b01) {
      auto fetch_opcode = FetchOpcode(ucode_data_[instr_offset * 3] & 0x1F);
      if (fetch_opcode == FetchOpcode::kVertexFetch) {
        auto& op = *reinterpret_cast<const VertexFetchInstruction*>(
            ucode_data_.data() + instr_offset * 3);
        GatherVertexFetchInformation(op, previous_vfetch_full,
                                     ucode_disasm_buffer);
      } else {
        auto& op = *reinterpret_cast<const TextureFetchInstruction*>(
            ucode_data_.data() + instr_offset * 3);
        GatherTextureFetchInformation(op, unique_texture_bindings,
                                      ucode_disasm_buffer);
      }
    } else {
      auto& op = *reinterpret_cast<const AluInstruction*>(ucode_data_.data() +
                                                          instr_offset * 3);
      GatherAluInstructionInformation(op, memexport_alloc_current_count,
                                      memexport_eA_written,
                                      ucode_disasm_buffer);
    }
  }
}

void Shader::GatherVertexFetchInformation(
    const VertexFetchInstruction& op,
    VertexFetchInstruction& previous_vfetch_full,
    StringBuffer& ucode_disasm_buffer) {
  ParsedVertexFetchInstruction fetch_instr;
  if (ParseVertexFetchInstruction(op, previous_vfetch_full, fetch_instr)) {
    previous_vfetch_full = op;
  }
  fetch_instr.Disassemble(&ucode_disasm_buffer);

  GatherFetchResultInformation(fetch_instr.result);

  // Don't bother setting up a binding for an instruction that fetches nothing.
  if (!fetch_instr.result.GetUsedResultComponents()) {
    return;
  }

  for (size_t i = 0; i < fetch_instr.operand_count; ++i) {
    GatherOperandInformation(fetch_instr.operands[i]);
  }

  // Try to allocate an attribute on an existing binding.
  // If no binding for this fetch slot is found create it.
  using VertexBinding = Shader::VertexBinding;
  VertexBinding::Attribute* attrib = nullptr;
  for (auto& vertex_binding : vertex_bindings_) {
    if (vertex_binding.fetch_constant == op.fetch_constant_index()) {
      // It may not hold that all strides are equal, but I hope it does.
      assert_true(!fetch_instr.attributes.stride ||
                  vertex_binding.stride_words == fetch_instr.attributes.stride);
      vertex_binding.attributes.push_back({});
      attrib = &vertex_binding.attributes.back();
      break;
    }
  }
  if (!attrib) {
    assert_not_zero(fetch_instr.attributes.stride);
    VertexBinding vertex_binding;
    vertex_binding.binding_index = int(vertex_bindings_.size());
    vertex_binding.fetch_constant = op.fetch_constant_index();
    vertex_binding.stride_words = fetch_instr.attributes.stride;
    vertex_binding.attributes.push_back({});
    vertex_bindings_.emplace_back(std::move(vertex_binding));
    attrib = &vertex_bindings_.back().attributes.back();
  }

  // Populate attribute.
  attrib->fetch_instr = fetch_instr;
}

void Shader::GatherTextureFetchInformation(const TextureFetchInstruction& op,
                                           uint32_t& unique_texture_bindings,
                                           StringBuffer& ucode_disasm_buffer) {
  TextureBinding binding;
  ParseTextureFetchInstruction(op, binding.fetch_instr);
  binding.fetch_instr.Disassemble(&ucode_disasm_buffer);

  GatherFetchResultInformation(binding.fetch_instr.result);
  for (size_t i = 0; i < binding.fetch_instr.operand_count; ++i) {
    GatherOperandInformation(binding.fetch_instr.operands[i]);
  }

  switch (op.opcode()) {
    case FetchOpcode::kSetTextureLod:
    case FetchOpcode::kSetTextureGradientsHorz:
    case FetchOpcode::kSetTextureGradientsVert:
      // Doesn't use bindings.
      return;
    default:
      // Continue.
      break;
  }
  binding.binding_index = -1;
  binding.fetch_constant = binding.fetch_instr.operands[1].storage_index;

  // Check and see if this fetch constant was previously used...
  for (auto& tex_binding : texture_bindings_) {
    if (tex_binding.fetch_constant == binding.fetch_constant) {
      binding.binding_index = tex_binding.binding_index;
      break;
    }
  }

  if (binding.binding_index == -1) {
    // Assign a unique binding index.
    binding.binding_index = unique_texture_bindings++;
  }

  texture_bindings_.emplace_back(std::move(binding));
}

void Shader::GatherAluInstructionInformation(
    const AluInstruction& op, uint32_t memexport_alloc_current_count,
    uint32_t& memexport_eA_written, StringBuffer& ucode_disasm_buffer) {
  ParsedAluInstruction instr;
  ParseAluInstruction(op, type(), instr);
  instr.Disassemble(&ucode_disasm_buffer);

  kills_pixels_ = kills_pixels_ ||
                  ucode::AluVectorOpcodeIsKill(op.vector_opcode()) ||
                  ucode::AluScalarOpcodeIsKill(op.scalar_opcode());

  GatherAluResultInformation(instr.vector_and_constant_result,
                             memexport_alloc_current_count);
  GatherAluResultInformation(instr.scalar_result,
                             memexport_alloc_current_count);
  for (size_t i = 0; i < instr.vector_operand_count; ++i) {
    GatherOperandInformation(instr.vector_operands[i]);
  }
  for (size_t i = 0; i < instr.scalar_operand_count; ++i) {
    GatherOperandInformation(instr.scalar_operands[i]);
  }

  // Store used memexport constants because CPU code needs addresses and sizes,
  // and also whether there have been writes to eA and eM# for register
  // allocation in shader translator implementations.
  // eA is (hopefully) always written to using:
  // mad eA, r#, const0100, c#
  // (though there are some exceptions, shaders in 4D5307E6 for some reason set
  // eA to zeros, but the swizzle of the constant is not .xyzw in this case, and
  // they don't write to eM#).
  // Export is done to vector_dest of the ucode instruction for both vector and
  // scalar operations - no need to check separately.
  if (instr.vector_and_constant_result.storage_target ==
          InstructionStorageTarget::kExportAddress &&
      memexport_alloc_current_count > 0 &&
      memexport_alloc_current_count <= Shader::kMaxMemExports) {
    uint32_t memexport_stream_constant = instr.GetMemExportStreamConstant();
    if (memexport_stream_constant != UINT32_MAX) {
      memexport_eA_written |= uint32_t(1)
                              << (memexport_alloc_current_count - 1);
      memexport_stream_constants_.insert(memexport_stream_constant);
    } else {
      XELOGE(
          "ShaderTranslator::GatherAluInstructionInformation: Couldn't extract "
          "memexport stream constant index");
    }
  }
}

void Shader::GatherOperandInformation(const InstructionOperand& operand) {
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      if (operand.storage_addressing_mode ==
          InstructionStorageAddressingMode::kStatic) {
        register_static_address_bound_ =
            std::max(register_static_address_bound_,
                     operand.storage_index + uint32_t(1));
      } else {
        uses_register_dynamic_addressing_ = true;
      }
      break;
    case InstructionStorageSource::kConstantFloat:
      if (operand.storage_addressing_mode ==
          InstructionStorageAddressingMode::kStatic) {
        // Store used float constants before translating so the
        // translator can use tightly packed indices if not dynamically
        // indexed.
        constant_register_map_.float_bitmap[operand.storage_index >> 6] |=
            uint64_t(1) << (operand.storage_index & 63);
      } else {
        constant_register_map_.float_dynamic_addressing = true;
      }
      break;
    default:
      break;
  }
}

void Shader::GatherFetchResultInformation(const InstructionResult& result) {
  if (!result.GetUsedWriteMask()) {
    return;
  }
  // Fetch instructions can't export - don't need the current memexport count
  // operand.
  assert_true(result.storage_target == InstructionStorageTarget::kRegister);
  if (result.storage_addressing_mode ==
      InstructionStorageAddressingMode::kStatic) {
    register_static_address_bound_ = std::max(
        register_static_address_bound_, result.storage_index + uint32_t(1));
  } else {
    uses_register_dynamic_addressing_ = true;
  }
}

void Shader::GatherAluResultInformation(
    const InstructionResult& result, uint32_t memexport_alloc_current_count) {
  if (!result.GetUsedWriteMask()) {
    return;
  }
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      if (result.storage_addressing_mode ==
          InstructionStorageAddressingMode::kStatic) {
        register_static_address_bound_ = std::max(
            register_static_address_bound_, result.storage_index + uint32_t(1));
      } else {
        uses_register_dynamic_addressing_ = true;
      }
      break;
    case InstructionStorageTarget::kExportData:
      if (memexport_alloc_current_count > 0 &&
          memexport_alloc_current_count <= Shader::kMaxMemExports) {
        memexport_eM_written_[memexport_alloc_current_count - 1] |=
            uint32_t(1) << result.storage_index;
      }
      break;
    case InstructionStorageTarget::kColor:
      writes_color_targets_ |= uint32_t(1) << result.storage_index;
      break;
    case InstructionStorageTarget::kDepth:
      writes_depth_ = true;
      break;
    default:
      break;
  }
}

ShaderTranslator::ShaderTranslator() = default;

ShaderTranslator::~ShaderTranslator() = default;

void ShaderTranslator::Reset() {
  errors_.clear();
  std::memset(&previous_vfetch_full_, 0, sizeof(previous_vfetch_full_));
}

bool ShaderTranslator::TranslateAnalyzedShader(
    Shader::Translation& translation) {
  const Shader& shader = translation.shader();
  assert_true(shader.is_ucode_analyzed());
  if (!shader.is_ucode_analyzed()) {
    XELOGE("AnalyzeUcode must be done on the shader before translation");
    return false;
  }
  translation_ = &translation;

  Reset();

  register_count_ = shader.register_static_address_bound();
  if (shader.uses_register_dynamic_addressing()) {
    // An array of registers at the end of the r# space may be dynamically
    // addressable - ensure enough space, as specified in SQ_PROGRAM_CNTL, is
    // allocated.
    register_count_ = std::max(register_count_, GetModificationRegisterCount());
  }

  StartTranslation();

  const uint32_t* ucode_dwords = shader.ucode_data().data();

  // TODO(Triang3l): Remove when the old SPIR-V shader translator is deleted.
  uint32_t cf_pair_index_bound = shader.cf_pair_index_bound();
  std::vector<ControlFlowInstruction> cf_instructions;
  for (uint32_t i = 0; i < cf_pair_index_bound; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_dwords + i * 3, cf_ab);
    cf_instructions.push_back(cf_ab[0]);
    cf_instructions.push_back(cf_ab[1]);
  }
  PreProcessControlFlowInstructions(cf_instructions);

  // Translate all instructions.
  const std::set<uint32_t>& label_addresses = shader.label_addresses();
  for (uint32_t i = 0; i < cf_pair_index_bound; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_dwords + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      uint32_t cf_index = i * 2 + j;
      cf_index_ = cf_index;
      if (label_addresses.find(cf_index) != label_addresses.end()) {
        ProcessLabel(cf_index);
      }
      ProcessControlFlowInstructionBegin(cf_index);
      TranslateControlFlowInstruction(cf_ab[j]);
      ProcessControlFlowInstructionEnd(cf_index);
    }
  }

  translation.errors_ = std::move(errors_);
  translation.translated_binary_ = CompleteTranslation();
  translation.is_translated_ = true;

  bool is_valid = true;
  for (const auto& error : translation.errors_) {
    if (error.is_fatal) {
      is_valid = false;
      break;
    }
  }
  translation.is_valid_ = is_valid;

  PostTranslation();

  // In case is_valid_ is modified by PostTranslation, reload.
  return translation.is_valid_;
}

void ShaderTranslator::EmitTranslationError(const char* message,
                                            bool is_fatal) {
  Shader::Error error;
  error.is_fatal = is_fatal;
  error.message = message;
  // TODO(benvanik): location information.
  errors_.push_back(std::move(error));
  XELOGE("Shader translation {}error: {}", is_fatal ? "fatal " : "", message);
}

void ShaderTranslator::TranslateControlFlowInstruction(
    const ControlFlowInstruction& cf) {
  switch (cf.opcode()) {
    case ControlFlowOpcode::kNop:
      ProcessControlFlowNopInstruction(cf_index_);
      break;
    case ControlFlowOpcode::kExec:
    case ControlFlowOpcode::kExecEnd: {
      ParsedExecInstruction instr;
      ParseControlFlowExec(cf.exec, cf_index_, instr);
      TranslateExecInstructions(instr);
    } break;
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPredClean:
    case ControlFlowOpcode::kCondExecPredCleanEnd: {
      ParsedExecInstruction instr;
      ParseControlFlowCondExec(cf.cond_exec, cf_index_, instr);
      TranslateExecInstructions(instr);
    } break;
    case ControlFlowOpcode::kCondExecPred:
    case ControlFlowOpcode::kCondExecPredEnd: {
      ParsedExecInstruction instr;
      ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index_, instr);
      TranslateExecInstructions(instr);
    } break;
    case ControlFlowOpcode::kLoopStart: {
      ParsedLoopStartInstruction instr;
      ParseControlFlowLoopStart(cf.loop_start, cf_index_, instr);
      ProcessLoopStartInstruction(instr);
    } break;
    case ControlFlowOpcode::kLoopEnd: {
      ParsedLoopEndInstruction instr;
      ParseControlFlowLoopEnd(cf.loop_end, cf_index_, instr);
      ProcessLoopEndInstruction(instr);
    } break;
    case ControlFlowOpcode::kCondCall: {
      ParsedCallInstruction instr;
      ParseControlFlowCondCall(cf.cond_call, cf_index_, instr);
      ProcessCallInstruction(instr);
    } break;
    case ControlFlowOpcode::kReturn: {
      ParsedReturnInstruction instr;
      ParseControlFlowReturn(cf.ret, cf_index_, instr);
      ProcessReturnInstruction(instr);
    } break;
    case ControlFlowOpcode::kCondJmp: {
      ParsedJumpInstruction instr;
      ParseControlFlowCondJmp(cf.cond_jmp, cf_index_, instr);
      ProcessJumpInstruction(instr);
    } break;
    case ControlFlowOpcode::kAlloc: {
      ParsedAllocInstruction instr;
      ParseControlFlowAlloc(cf.alloc, cf_index_, is_vertex_shader(), instr);
      ProcessAllocInstruction(instr);
    } break;
    case ControlFlowOpcode::kMarkVsFetchDone:
      break;
    default:
      assert_unhandled_case(cf.opcode);
      break;
  }
  // TODO(benvanik): return if (DoesControlFlowOpcodeEndShader(cf.opcode()))?
}

void ParseControlFlowExec(const ControlFlowExecInstruction& cf,
                          uint32_t cf_index, ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.opcode();
  instr.opcode_name =
      cf.opcode() == ControlFlowOpcode::kExecEnd ? "exece" : "exec";
  instr.instruction_address = cf.address();
  instr.instruction_count = cf.count();
  instr.type = ParsedExecInstruction::Type::kUnconditional;
  instr.is_end = cf.opcode() == ControlFlowOpcode::kExecEnd;
  instr.clean = cf.clean();
  instr.is_yield = cf.is_yield();
  instr.sequence = cf.sequence();
}

void ParseControlFlowCondExec(const ControlFlowCondExecInstruction& cf,
                              uint32_t cf_index, ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.opcode();
  instr.opcode_name = "cexec";
  switch (cf.opcode()) {
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPredCleanEnd:
      instr.opcode_name = "cexece";
      instr.is_end = true;
      break;
    default:
      break;
  }
  instr.instruction_address = cf.address();
  instr.instruction_count = cf.count();
  instr.type = ParsedExecInstruction::Type::kConditional;
  instr.bool_constant_index = cf.bool_address();
  instr.condition = cf.condition();
  switch (cf.opcode()) {
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
      instr.clean = false;
      break;
    default:
      break;
  }
  instr.is_yield = cf.is_yield();
  instr.sequence = cf.sequence();
}

void ParseControlFlowCondExecPred(const ControlFlowCondExecPredInstruction& cf,
                                  uint32_t cf_index,
                                  ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.opcode();
  instr.opcode_name =
      cf.opcode() == ControlFlowOpcode::kCondExecPredEnd ? "exece" : "exec";
  instr.instruction_address = cf.address();
  instr.instruction_count = cf.count();
  instr.type = ParsedExecInstruction::Type::kPredicated;
  instr.condition = cf.condition();
  instr.is_end = cf.opcode() == ControlFlowOpcode::kCondExecPredEnd;
  instr.clean = cf.clean();
  instr.is_yield = cf.is_yield();
  instr.sequence = cf.sequence();
}

void ParseControlFlowLoopStart(const ControlFlowLoopStartInstruction& cf,
                               uint32_t cf_index,
                               ParsedLoopStartInstruction& instr) {
  instr.dword_index = cf_index;
  instr.loop_constant_index = cf.loop_id();
  instr.is_repeat = cf.is_repeat();
  instr.loop_skip_address = cf.address();
}

void ParseControlFlowLoopEnd(const ControlFlowLoopEndInstruction& cf,
                             uint32_t cf_index,
                             ParsedLoopEndInstruction& instr) {
  instr.dword_index = cf_index;
  instr.is_predicated_break = cf.is_predicated_break();
  instr.predicate_condition = cf.condition();
  instr.loop_constant_index = cf.loop_id();
  instr.loop_body_address = cf.address();
}

void ParseControlFlowCondCall(const ControlFlowCondCallInstruction& cf,
                              uint32_t cf_index, ParsedCallInstruction& instr) {
  instr.dword_index = cf_index;
  instr.target_address = cf.address();
  if (cf.is_unconditional()) {
    instr.type = ParsedCallInstruction::Type::kUnconditional;
  } else if (cf.is_predicated()) {
    instr.type = ParsedCallInstruction::Type::kPredicated;
    instr.condition = cf.condition();
  } else {
    instr.type = ParsedCallInstruction::Type::kConditional;
    instr.bool_constant_index = cf.bool_address();
    instr.condition = cf.condition();
  }
}

void ParseControlFlowReturn(const ControlFlowReturnInstruction& cf,
                            uint32_t cf_index, ParsedReturnInstruction& instr) {
  instr.dword_index = cf_index;
}

void ParseControlFlowCondJmp(const ControlFlowCondJmpInstruction& cf,
                             uint32_t cf_index, ParsedJumpInstruction& instr) {
  instr.dword_index = cf_index;
  instr.target_address = cf.address();
  if (cf.is_unconditional()) {
    instr.type = ParsedJumpInstruction::Type::kUnconditional;
  } else if (cf.is_predicated()) {
    instr.type = ParsedJumpInstruction::Type::kPredicated;
    instr.condition = cf.condition();
  } else {
    instr.type = ParsedJumpInstruction::Type::kConditional;
    instr.bool_constant_index = cf.bool_address();
    instr.condition = cf.condition();
  }
}

void ParseControlFlowAlloc(const ControlFlowAllocInstruction& cf,
                           uint32_t cf_index, bool is_vertex_shader,
                           ParsedAllocInstruction& instr) {
  instr.dword_index = cf_index;
  instr.type = cf.alloc_type();
  instr.count = cf.size();
  instr.is_vertex_shader = is_vertex_shader;
}

void ShaderTranslator::TranslateExecInstructions(
    const ParsedExecInstruction& instr) {
  ProcessExecInstructionBegin(instr);
  const uint32_t* ucode_dwords = current_shader().ucode_data().data();
  uint32_t sequence = instr.sequence;
  for (uint32_t instr_offset = instr.instruction_address;
       instr_offset < instr.instruction_address + instr.instruction_count;
       ++instr_offset, sequence >>= 2) {
    if (sequence & 0b01) {
      auto fetch_opcode =
          static_cast<FetchOpcode>(ucode_dwords[instr_offset * 3] & 0x1F);
      if (fetch_opcode == FetchOpcode::kVertexFetch) {
        auto& op = *reinterpret_cast<const VertexFetchInstruction*>(
            ucode_dwords + instr_offset * 3);
        ParsedVertexFetchInstruction vfetch_instr;
        if (ParseVertexFetchInstruction(op, previous_vfetch_full_,
                                        vfetch_instr)) {
          previous_vfetch_full_ = op;
        }
        ProcessVertexFetchInstruction(vfetch_instr);
      } else {
        auto& op = *reinterpret_cast<const TextureFetchInstruction*>(
            ucode_dwords + instr_offset * 3);
        ParsedTextureFetchInstruction tfetch_instr;
        ParseTextureFetchInstruction(op, tfetch_instr);
        ProcessTextureFetchInstruction(tfetch_instr);
      }
    } else {
      auto& op = *reinterpret_cast<const AluInstruction*>(ucode_dwords +
                                                          instr_offset * 3);
      ParsedAluInstruction alu_instr;
      ParseAluInstruction(op, current_shader().type(), alu_instr);
      ProcessAluInstruction(alu_instr);
    }
  }
  ProcessExecInstructionEnd(instr);
}

static void ParseFetchInstructionResult(uint32_t dest, uint32_t swizzle,
                                        bool is_relative,
                                        InstructionResult& result) {
  result.storage_target = InstructionStorageTarget::kRegister;
  result.storage_index = dest;
  result.is_clamped = false;
  result.storage_addressing_mode =
      is_relative ? InstructionStorageAddressingMode::kAddressRelative
                  : InstructionStorageAddressingMode::kStatic;
  result.original_write_mask = 0b1111;
  for (int i = 0; i < 4; ++i) {
    switch (swizzle & 0x7) {
      case 4:
      case 6:
        result.components[i] = SwizzleSource::k0;
        break;
      case 5:
        result.components[i] = SwizzleSource::k1;
        break;
      case 7:
        result.original_write_mask &= ~uint32_t(1 << i);
        break;
      default:
        result.components[i] = GetSwizzleFromComponentIndex(swizzle & 0x3);
    }
    swizzle >>= 3;
  }
}

bool ParseVertexFetchInstruction(const VertexFetchInstruction& op,
                                 const VertexFetchInstruction& previous_full_op,
                                 ParsedVertexFetchInstruction& instr) {
  instr.opcode = FetchOpcode::kVertexFetch;
  instr.opcode_name = op.is_mini_fetch() ? "vfetch_mini" : "vfetch_full";
  instr.is_mini_fetch = op.is_mini_fetch();
  instr.is_predicated = op.is_predicated();
  instr.predicate_condition = op.predicate_condition();

  ParseFetchInstructionResult(op.dest(), op.dest_swizzle(),
                              op.is_dest_relative(), instr.result);

  // Reuse previous vfetch_full if this is a mini.
  const auto& full_op = op.is_mini_fetch() ? previous_full_op : op;
  auto& src_op = instr.operands[instr.operand_count++];
  src_op.storage_source = InstructionStorageSource::kRegister;
  src_op.storage_index = full_op.src();
  src_op.storage_addressing_mode =
      full_op.is_src_relative()
          ? InstructionStorageAddressingMode::kAddressRelative
          : InstructionStorageAddressingMode::kStatic;
  src_op.is_negated = false;
  src_op.is_absolute_value = false;
  src_op.component_count = 1;
  uint32_t swizzle = full_op.src_swizzle();
  for (uint32_t j = 0; j < src_op.component_count; ++j, swizzle >>= 2) {
    src_op.components[j] = GetSwizzleFromComponentIndex(swizzle & 0x3);
  }

  auto& const_op = instr.operands[instr.operand_count++];
  const_op.storage_source = InstructionStorageSource::kVertexFetchConstant;
  const_op.storage_index = full_op.fetch_constant_index();

  instr.attributes.data_format = op.data_format();
  instr.attributes.offset = op.offset();
  instr.attributes.stride = full_op.stride();
  instr.attributes.exp_adjust = op.exp_adjust();
  instr.attributes.prefetch_count = op.prefetch_count();
  instr.attributes.is_index_rounded = op.is_index_rounded();
  instr.attributes.is_signed = op.is_signed();
  instr.attributes.is_integer = !op.is_normalized();
  instr.attributes.signed_rf_mode = op.signed_rf_mode();

  return !op.is_mini_fetch();
}

void ParseTextureFetchInstruction(const TextureFetchInstruction& op,
                                  ParsedTextureFetchInstruction& instr) {
  struct TextureFetchOpcodeInfo {
    const char* name;
    bool has_dest;
    bool has_const;
    bool has_attributes;
    uint32_t override_component_count;
  } opcode_info;
  switch (op.opcode()) {
    case FetchOpcode::kTextureFetch: {
      static const char* kNames[] = {"tfetch1D", "tfetch2D", "tfetch3D",
                                     "tfetchCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true,
                     0};
    } break;
    case FetchOpcode::kGetTextureBorderColorFrac: {
      static const char* kNames[] = {"getBCF1D", "getBCF2D", "getBCF3D",
                                     "getBCFCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true,
                     0};
    } break;
    case FetchOpcode::kGetTextureComputedLod: {
      static const char* kNames[] = {"getCompTexLOD1D", "getCompTexLOD2D",
                                     "getCompTexLOD3D", "getCompTexLODCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true,
                     0};
    } break;
    case FetchOpcode::kGetTextureGradients:
      opcode_info = {"getGradients", true, true, true, 2};
      break;
    case FetchOpcode::kGetTextureWeights: {
      static const char* kNames[] = {"getWeights1D", "getWeights2D",
                                     "getWeights3D", "getWeightsCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true,
                     0};
    } break;
    case FetchOpcode::kSetTextureLod:
      opcode_info = {"setTexLOD", false, false, false, 1};
      break;
    case FetchOpcode::kSetTextureGradientsHorz:
      opcode_info = {"setGradientH", false, false, false, 3};
      break;
    case FetchOpcode::kSetTextureGradientsVert:
      opcode_info = {"setGradientV", false, false, false, 3};
      break;
    default:
      assert_unhandled_case(fetch_opcode);
      return;
  }

  instr.opcode = op.opcode();
  instr.opcode_name = opcode_info.name;
  instr.dimension = op.dimension();
  instr.is_predicated = op.is_predicated();
  instr.predicate_condition = op.predicate_condition();

  if (opcode_info.has_dest) {
    ParseFetchInstructionResult(op.dest(), op.dest_swizzle(),
                                op.is_dest_relative(), instr.result);
  } else {
    instr.result.storage_target = InstructionStorageTarget::kNone;
  }

  auto& src_op = instr.operands[instr.operand_count++];
  src_op.storage_source = InstructionStorageSource::kRegister;
  src_op.storage_index = op.src();
  src_op.storage_addressing_mode =
      op.is_src_relative() ? InstructionStorageAddressingMode::kAddressRelative
                           : InstructionStorageAddressingMode::kStatic;
  src_op.is_negated = false;
  src_op.is_absolute_value = false;
  src_op.component_count =
      opcode_info.override_component_count
          ? opcode_info.override_component_count
          : xenos::GetFetchOpDimensionComponentCount(op.dimension());
  uint32_t swizzle = op.src_swizzle();
  for (uint32_t j = 0; j < src_op.component_count; ++j, swizzle >>= 2) {
    src_op.components[j] = GetSwizzleFromComponentIndex(swizzle & 0x3);
  }

  if (opcode_info.has_const) {
    auto& const_op = instr.operands[instr.operand_count++];
    const_op.storage_source = InstructionStorageSource::kTextureFetchConstant;
    const_op.storage_index = op.fetch_constant_index();
  }

  if (opcode_info.has_attributes) {
    instr.attributes.fetch_valid_only = op.fetch_valid_only();
    instr.attributes.unnormalized_coordinates = op.unnormalized_coordinates();
    instr.attributes.mag_filter = op.mag_filter();
    instr.attributes.min_filter = op.min_filter();
    instr.attributes.mip_filter = op.mip_filter();
    instr.attributes.aniso_filter = op.aniso_filter();
    instr.attributes.vol_mag_filter = op.vol_mag_filter();
    instr.attributes.vol_min_filter = op.vol_min_filter();
    instr.attributes.use_computed_lod = op.use_computed_lod();
    instr.attributes.use_register_lod = op.use_register_lod();
    instr.attributes.use_register_gradients = op.use_register_gradients();
    instr.attributes.lod_bias = op.lod_bias();
    instr.attributes.offset_x = op.offset_x();
    instr.attributes.offset_y = op.offset_y();
    instr.attributes.offset_z = op.offset_z();
  }
}

uint32_t ParsedTextureFetchInstruction::GetNonZeroResultComponents() const {
  uint32_t components = 0b0000;
  switch (opcode) {
    case FetchOpcode::kTextureFetch:
    case FetchOpcode::kGetTextureGradients:
      components = 0b1111;
      break;
    case FetchOpcode::kGetTextureBorderColorFrac:
      components = 0b0001;
      break;
    case FetchOpcode::kGetTextureComputedLod:
      // Not checking if the MipFilter is basemap because XNA doesn't accept
      // MipFilter for getCompTexLOD.
      components = 0b0001;
      break;
    case FetchOpcode::kGetTextureWeights:
      // FIXME(Triang3l): Not caring about mag/min filters currently for
      // simplicity. It's very unlikely that this instruction is ever seriously
      // used to retrieve weights of zero though.
      switch (dimension) {
        case xenos::FetchOpDimension::k1D:
          components = 0b1001;
          break;
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube:
          // TODO(Triang3l): Is the depth lerp factor always 0 for cube maps?
          components = 0b1011;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          components = 0b1111;
          break;
      }
      if (attributes.mip_filter == xenos::TextureFilter::kBaseMap ||
          attributes.mip_filter == xenos::TextureFilter::kPoint) {
        components &= ~uint32_t(0b1000);
      }
      break;
    case FetchOpcode::kSetTextureLod:
    case FetchOpcode::kSetTextureGradientsHorz:
    case FetchOpcode::kSetTextureGradientsVert:
      components = 0b0000;
      break;
    default:
      assert_unhandled_case(opcode);
  }
  return result.GetUsedResultComponents() & components;
}

struct AluOpcodeInfo {
  const char* name;
  uint32_t argument_count;
  uint32_t src_swizzle_component_count;
};

static const AluOpcodeInfo alu_vector_opcode_infos[0x20] = {
    {"add", 2, 4},           // 0
    {"mul", 2, 4},           // 1
    {"max", 2, 4},           // 2
    {"min", 2, 4},           // 3
    {"seq", 2, 4},           // 4
    {"sgt", 2, 4},           // 5
    {"sge", 2, 4},           // 6
    {"sne", 2, 4},           // 7
    {"frc", 1, 4},           // 8
    {"trunc", 1, 4},         // 9
    {"floor", 1, 4},         // 10
    {"mad", 3, 4},           // 11
    {"cndeq", 3, 4},         // 12
    {"cndge", 3, 4},         // 13
    {"cndgt", 3, 4},         // 14
    {"dp4", 2, 4},           // 15
    {"dp3", 2, 4},           // 16
    {"dp2add", 3, 4},        // 17
    {"cube", 2, 4},          // 18
    {"max4", 1, 4},          // 19
    {"setp_eq_push", 2, 4},  // 20
    {"setp_ne_push", 2, 4},  // 21
    {"setp_gt_push", 2, 4},  // 22
    {"setp_ge_push", 2, 4},  // 23
    {"kill_eq", 2, 4},       // 24
    {"kill_gt", 2, 4},       // 25
    {"kill_ge", 2, 4},       // 26
    {"kill_ne", 2, 4},       // 27
    {"dst", 2, 4},           // 28
    {"maxa", 2, 4},          // 29
};

static const AluOpcodeInfo alu_scalar_opcode_infos[0x40] = {
    {"adds", 1, 2},         // 0
    {"adds_prev", 1, 1},    // 1
    {"muls", 1, 2},         // 2
    {"muls_prev", 1, 1},    // 3
    {"muls_prev2", 1, 2},   // 4
    {"maxs", 1, 2},         // 5
    {"mins", 1, 2},         // 6
    {"seqs", 1, 1},         // 7
    {"sgts", 1, 1},         // 8
    {"sges", 1, 1},         // 9
    {"snes", 1, 1},         // 10
    {"frcs", 1, 1},         // 11
    {"truncs", 1, 1},       // 12
    {"floors", 1, 1},       // 13
    {"exp", 1, 1},          // 14
    {"logc", 1, 1},         // 15
    {"log", 1, 1},          // 16
    {"rcpc", 1, 1},         // 17
    {"rcpf", 1, 1},         // 18
    {"rcp", 1, 1},          // 19
    {"rsqc", 1, 1},         // 20
    {"rsqf", 1, 1},         // 21
    {"rsq", 1, 1},          // 22
    {"maxas", 1, 2},        // 23
    {"maxasf", 1, 2},       // 24
    {"subs", 1, 2},         // 25
    {"subs_prev", 1, 1},    // 26
    {"setp_eq", 1, 1},      // 27
    {"setp_ne", 1, 1},      // 28
    {"setp_gt", 1, 1},      // 29
    {"setp_ge", 1, 1},      // 30
    {"setp_inv", 1, 1},     // 31
    {"setp_pop", 1, 1},     // 32
    {"setp_clr", 0, 0},     // 33
    {"setp_rstr", 1, 1},    // 34
    {"kills_eq", 1, 1},     // 35
    {"kills_gt", 1, 1},     // 36
    {"kills_ge", 1, 1},     // 37
    {"kills_ne", 1, 1},     // 38
    {"kills_one", 1, 1},    // 39
    {"sqrt", 1, 1},         // 40
    {"UNKNOWN", 0, 0},      // 41
    {"mulsc", 2, 1},        // 42
    {"mulsc", 2, 1},        // 43
    {"addsc", 2, 1},        // 44
    {"addsc", 2, 1},        // 45
    {"subsc", 2, 1},        // 46
    {"subsc", 2, 1},        // 47
    {"sin", 1, 1},          // 48
    {"cos", 1, 1},          // 49
    {"retain_prev", 0, 0},  // 50
};

static void ParseAluInstructionOperand(const AluInstruction& op, uint32_t i,
                                       uint32_t swizzle_component_count,
                                       InstructionOperand& out_op) {
  int const_slot = 0;
  switch (i) {
    case 2:
      const_slot = op.src_is_temp(1) ? 0 : 1;
      break;
    case 3:
      const_slot = op.src_is_temp(1) && op.src_is_temp(2) ? 0 : 1;
      break;
  }
  out_op.is_negated = op.src_negate(i);
  uint32_t reg = op.src_reg(i);
  if (op.src_is_temp(i)) {
    out_op.storage_source = InstructionStorageSource::kRegister;
    out_op.storage_index = reg & 0x1F;
    out_op.is_absolute_value = (reg & 0x80) == 0x80;
    out_op.storage_addressing_mode =
        (reg & 0x40) ? InstructionStorageAddressingMode::kAddressRelative
                     : InstructionStorageAddressingMode::kStatic;
  } else {
    out_op.storage_source = InstructionStorageSource::kConstantFloat;
    out_op.storage_index = reg;
    if ((const_slot == 0 && op.is_const_0_addressed()) ||
        (const_slot == 1 && op.is_const_1_addressed())) {
      if (op.is_address_relative()) {
        out_op.storage_addressing_mode =
            InstructionStorageAddressingMode::kAddressAbsolute;
      } else {
        out_op.storage_addressing_mode =
            InstructionStorageAddressingMode::kAddressRelative;
      }
    } else {
      out_op.storage_addressing_mode =
          InstructionStorageAddressingMode::kStatic;
    }
    out_op.is_absolute_value = op.abs_constants();
  }
  out_op.component_count = swizzle_component_count;
  uint32_t swizzle = op.src_swizzle(i);
  if (swizzle_component_count == 1) {
    uint32_t a = ((swizzle >> 6) + 3) & 0x3;
    out_op.components[0] = GetSwizzleFromComponentIndex(a);
  } else if (swizzle_component_count == 2) {
    uint32_t a = ((swizzle >> 6) + 3) & 0x3;
    uint32_t b = ((swizzle >> 0) + 0) & 0x3;
    out_op.components[0] = GetSwizzleFromComponentIndex(a);
    out_op.components[1] = GetSwizzleFromComponentIndex(b);
  } else if (swizzle_component_count == 3) {
    assert_always();
  } else if (swizzle_component_count == 4) {
    for (uint32_t j = 0; j < swizzle_component_count; ++j, swizzle >>= 2) {
      out_op.components[j] = GetSwizzleFromComponentIndex((swizzle + j) & 0x3);
    }
  }
}

static void ParseAluInstructionOperandSpecial(
    const AluInstruction& op, InstructionStorageSource storage_source,
    uint32_t reg, bool negate, int const_slot, uint32_t component_index,
    InstructionOperand& out_op) {
  out_op.is_negated = negate;
  out_op.is_absolute_value = op.abs_constants();
  out_op.storage_source = storage_source;
  if (storage_source == InstructionStorageSource::kRegister) {
    out_op.storage_index = reg & 0x7F;
    out_op.storage_addressing_mode = InstructionStorageAddressingMode::kStatic;
  } else {
    out_op.storage_index = reg;
    if ((const_slot == 0 && op.is_const_0_addressed()) ||
        (const_slot == 1 && op.is_const_1_addressed())) {
      if (op.is_address_relative()) {
        out_op.storage_addressing_mode =
            InstructionStorageAddressingMode::kAddressAbsolute;
      } else {
        out_op.storage_addressing_mode =
            InstructionStorageAddressingMode::kAddressRelative;
      }
    } else {
      out_op.storage_addressing_mode =
          InstructionStorageAddressingMode::kStatic;
    }
  }
  out_op.component_count = 1;
  out_op.components[0] = GetSwizzleFromComponentIndex(component_index);
}

bool ParsedAluInstruction::IsVectorOpDefaultNop() const {
  if (vector_opcode != ucode::AluVectorOpcode::kMax ||
      vector_and_constant_result.original_write_mask ||
      vector_and_constant_result.is_clamped ||
      vector_operands[0].storage_source !=
          InstructionStorageSource::kRegister ||
      vector_operands[0].storage_index != 0 ||
      vector_operands[0].storage_addressing_mode !=
          InstructionStorageAddressingMode::kStatic ||
      vector_operands[0].is_negated || vector_operands[0].is_absolute_value ||
      !vector_operands[0].IsStandardSwizzle() ||
      vector_operands[1].storage_source !=
          InstructionStorageSource::kRegister ||
      vector_operands[1].storage_index != 0 ||
      vector_operands[1].storage_addressing_mode !=
          InstructionStorageAddressingMode::kStatic ||
      vector_operands[1].is_negated || vector_operands[1].is_absolute_value ||
      !vector_operands[1].IsStandardSwizzle()) {
    return false;
  }
  if (vector_and_constant_result.storage_target ==
      InstructionStorageTarget::kRegister) {
    if (vector_and_constant_result.storage_index != 0 ||
        vector_and_constant_result.storage_addressing_mode !=
            InstructionStorageAddressingMode::kStatic) {
      return false;
    }
  } else {
    // In case both vector and scalar operations are nop, still need to write
    // somewhere that it's an export, not mov r0._, r0 + retain_prev r0._.
    // Accurate round trip is possible only if the target is o0 or oC0, because
    // if the total write mask is empty, the XNA assembler forces the
    // destination to be o0/oC0, but this doesn't really matter in this case.
    if (IsScalarOpDefaultNop()) {
      return false;
    }
  }
  return true;
}

void ParseAluInstruction(const AluInstruction& op,
                         xenos::ShaderType shader_type,
                         ParsedAluInstruction& instr) {
  instr.is_predicated = op.is_predicated();
  instr.predicate_condition = op.predicate_condition();

  bool is_export = op.is_export();

  InstructionStorageTarget storage_target = InstructionStorageTarget::kRegister;
  uint32_t storage_index_export = 0;
  if (is_export) {
    storage_target = InstructionStorageTarget::kNone;
    // Both vector and scalar operation export to vector_dest.
    ExportRegister export_register = ExportRegister(op.vector_dest());
    if (export_register == ExportRegister::kExportAddress) {
      storage_target = InstructionStorageTarget::kExportAddress;
    } else if (export_register >= ExportRegister::kExportData0 &&
               export_register <= ExportRegister::kExportData4) {
      storage_target = InstructionStorageTarget::kExportData;
      storage_index_export =
          uint32_t(export_register) - uint32_t(ExportRegister::kExportData0);
    } else if (shader_type == xenos::ShaderType::kVertex) {
      if (export_register >= ExportRegister::kVSInterpolator0 &&
          export_register <= ExportRegister::kVSInterpolator15) {
        storage_target = InstructionStorageTarget::kInterpolator;
        storage_index_export = uint32_t(export_register) -
                               uint32_t(ExportRegister::kVSInterpolator0);
      } else if (export_register == ExportRegister::kVSPosition) {
        storage_target = InstructionStorageTarget::kPosition;
      } else if (export_register ==
                 ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
        storage_target = InstructionStorageTarget::kPointSizeEdgeFlagKillVertex;
      }
    } else if (shader_type == xenos::ShaderType::kPixel) {
      if (export_register >= ExportRegister::kPSColor0 &&
          export_register <= ExportRegister::kPSColor3) {
        storage_target = InstructionStorageTarget::kColor;
        storage_index_export =
            uint32_t(export_register) - uint32_t(ExportRegister::kPSColor0);
      } else if (export_register == ExportRegister::kPSDepth) {
        storage_target = InstructionStorageTarget::kDepth;
      }
    }
    if (storage_target == InstructionStorageTarget::kNone) {
      assert_always();
      XELOGE(
          "ShaderTranslator::ParseAluInstruction: Unsupported write to export "
          "{}",
          uint32_t(export_register));
    }
  }

  // Vector operation and constant 0/1 writes.

  instr.vector_opcode = op.vector_opcode();
  const auto& vector_opcode_info =
      alu_vector_opcode_infos[uint32_t(instr.vector_opcode)];
  instr.vector_opcode_name = vector_opcode_info.name;

  instr.vector_and_constant_result.storage_target = storage_target;
  instr.vector_and_constant_result.storage_addressing_mode =
      InstructionStorageAddressingMode::kStatic;
  if (is_export) {
    instr.vector_and_constant_result.storage_index = storage_index_export;
  } else {
    instr.vector_and_constant_result.storage_index = op.vector_dest();
    if (op.is_vector_dest_relative()) {
      instr.vector_and_constant_result.storage_addressing_mode =
          InstructionStorageAddressingMode::kAddressRelative;
    }
  }
  instr.vector_and_constant_result.is_clamped = op.vector_clamp();
  uint32_t constant_0_mask = op.GetConstant0WriteMask();
  uint32_t constant_1_mask = op.GetConstant1WriteMask();
  instr.vector_and_constant_result.original_write_mask =
      op.GetVectorOpResultWriteMask() | constant_0_mask | constant_1_mask;
  for (uint32_t i = 0; i < 4; ++i) {
    SwizzleSource component = GetSwizzleFromComponentIndex(i);
    if (constant_0_mask & (1 << i)) {
      component = SwizzleSource::k0;
    } else if (constant_1_mask & (1 << i)) {
      component = SwizzleSource::k1;
    }
    instr.vector_and_constant_result.components[i] = component;
  }

  instr.vector_operand_count = vector_opcode_info.argument_count;
  for (uint32_t i = 0; i < instr.vector_operand_count; ++i) {
    InstructionOperand& vector_operand = instr.vector_operands[i];
    ParseAluInstructionOperand(op, i + 1,
                               vector_opcode_info.src_swizzle_component_count,
                               vector_operand);
  }

  // Scalar operation.

  instr.scalar_opcode = op.scalar_opcode();
  const auto& scalar_opcode_info =
      alu_scalar_opcode_infos[uint32_t(instr.scalar_opcode)];
  instr.scalar_opcode_name = scalar_opcode_info.name;

  instr.scalar_result.storage_target = storage_target;
  instr.scalar_result.storage_addressing_mode =
      InstructionStorageAddressingMode::kStatic;
  if (is_export) {
    instr.scalar_result.storage_index = storage_index_export;
  } else {
    instr.scalar_result.storage_index = op.scalar_dest();
    if (op.is_scalar_dest_relative()) {
      instr.scalar_result.storage_addressing_mode =
          InstructionStorageAddressingMode::kAddressRelative;
    }
  }
  instr.scalar_result.is_clamped = op.scalar_clamp();
  instr.scalar_result.original_write_mask = op.GetScalarOpResultWriteMask();
  for (uint32_t i = 0; i < 4; ++i) {
    instr.scalar_result.components[i] = GetSwizzleFromComponentIndex(i);
  }

  instr.scalar_operand_count = scalar_opcode_info.argument_count;
  if (instr.scalar_operand_count) {
    if (instr.scalar_operand_count == 1) {
      ParseAluInstructionOperand(op, 3,
                                 scalar_opcode_info.src_swizzle_component_count,
                                 instr.scalar_operands[0]);
    } else {
      uint32_t src3_swizzle = op.src_swizzle(3);
      uint32_t component_a = ((src3_swizzle >> 6) + 3) & 0x3;
      uint32_t component_b = ((src3_swizzle >> 0) + 0) & 0x3;
      uint32_t reg2 = (src3_swizzle & 0x3C) | (op.src_is_temp(3) << 1) |
                      (static_cast<int>(op.scalar_opcode()) & 1);
      int const_slot = (op.src_is_temp(1) || op.src_is_temp(2)) ? 1 : 0;

      ParseAluInstructionOperandSpecial(
          op, InstructionStorageSource::kConstantFloat, op.src_reg(3),
          op.src_negate(3), 0, component_a, instr.scalar_operands[0]);

      ParseAluInstructionOperandSpecial(op, InstructionStorageSource::kRegister,
                                        reg2, op.src_negate(3), const_slot,
                                        component_b, instr.scalar_operands[1]);
    }
  }
}

bool ParsedAluInstruction::IsScalarOpDefaultNop() const {
  if (scalar_opcode != ucode::AluScalarOpcode::kRetainPrev ||
      scalar_result.original_write_mask || scalar_result.is_clamped) {
    return false;
  }
  if (scalar_result.storage_target == InstructionStorageTarget::kRegister) {
    if (scalar_result.storage_index != 0 ||
        scalar_result.storage_addressing_mode !=
            InstructionStorageAddressingMode::kStatic) {
      return false;
    }
  }
  // For exports, if both are nop, the vector operation will be kept to state in
  // the microcode that the destination in the microcode is an export.
  return true;
}

bool ParsedAluInstruction::IsNop() const {
  return scalar_opcode == ucode::AluScalarOpcode::kRetainPrev &&
         !scalar_result.GetUsedWriteMask() &&
         !vector_and_constant_result.GetUsedWriteMask() &&
         !ucode::AluVectorOpHasSideEffects(vector_opcode);
}

uint32_t ParsedAluInstruction::GetMemExportStreamConstant() const {
  if (vector_and_constant_result.storage_target ==
          InstructionStorageTarget::kExportAddress &&
      vector_opcode == ucode::AluVectorOpcode::kMad &&
      vector_and_constant_result.GetUsedResultComponents() == 0b1111 &&
      !vector_and_constant_result.is_clamped &&
      vector_operands[2].storage_source ==
          InstructionStorageSource::kConstantFloat &&
      vector_operands[2].storage_addressing_mode ==
          InstructionStorageAddressingMode::kStatic &&
      vector_operands[2].IsStandardSwizzle() &&
      !vector_operands[2].is_negated && !vector_operands[2].is_absolute_value) {
    return vector_operands[2].storage_index;
  }
  return UINT32_MAX;
}

}  // namespace gpu
}  // namespace xe
