#include "sf/psx/r3000_runtime.hpp"

#include <bit>
#include <cstdint>
#include <limits>

namespace sf::psx {
namespace {

enum class FastOperation : std::uint8_t {
  none,
  sll,
  srl,
  sra,
  sllv,
  srlv,
  srav,
  jr,
  jalr,
  mfhi,
  mthi,
  mflo,
  mtlo,
  mult,
  multu,
  div,
  divu,
  add,
  addu,
  sub,
  subu,
  and_,
  or_,
  xor_,
  nor,
  slt,
  sltu,
  bltz,
  bgez,
  bltzal,
  bgezal,
  j,
  jal,
  beq,
  bne,
  blez,
  bgtz,
  addi,
  addiu,
  slti,
  sltiu,
  andi,
  ori,
  xori,
  lui,
  lb,
  lbu,
  lh,
  lhu,
  lw,
  sb,
  sh,
  sw,
};

constexpr bool memoryOperation(FastOperation operation) noexcept {
  return operation == FastOperation::lb || operation == FastOperation::lbu ||
         operation == FastOperation::lh || operation == FastOperation::lhu ||
         operation == FastOperation::lw || operation == FastOperation::sb ||
         operation == FastOperation::sh || operation == FastOperation::sw;
}

constexpr bool writesRegister(FastOperation operation) noexcept {
  switch (operation) {
  case FastOperation::sll:
  case FastOperation::srl:
  case FastOperation::sra:
  case FastOperation::sllv:
  case FastOperation::srlv:
  case FastOperation::srav:
  case FastOperation::jalr:
  case FastOperation::mfhi:
  case FastOperation::mflo:
  case FastOperation::add:
  case FastOperation::addu:
  case FastOperation::sub:
  case FastOperation::subu:
  case FastOperation::and_:
  case FastOperation::or_:
  case FastOperation::xor_:
  case FastOperation::nor:
  case FastOperation::slt:
  case FastOperation::sltu:
  case FastOperation::bltzal:
  case FastOperation::bgezal:
  case FastOperation::jal:
  case FastOperation::addi:
  case FastOperation::addiu:
  case FastOperation::slti:
  case FastOperation::sltiu:
  case FastOperation::andi:
  case FastOperation::ori:
  case FastOperation::xori:
  case FastOperation::lui:
    return true;
  default:
    return false;
  }
}

constexpr std::uint32_t signExtend16(std::uint32_t value) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(
      static_cast<std::int16_t>(static_cast<std::uint16_t>(value))));
}

constexpr std::int32_t asSigned(std::uint32_t value) noexcept {
  return std::bit_cast<std::int32_t>(value);
}

constexpr std::uint32_t arithmeticShiftRight(std::uint32_t value,
                                             std::uint32_t amount) noexcept {
  amount &= 31U;
  if (amount == 0U) {
    return value;
  }
  const auto shifted = value >> amount;
  return (value & 0x80000000U) == 0U
             ? shifted
             : shifted | (~std::uint32_t{} << (32U - amount));
}

bool addOverflows(std::uint32_t left, std::uint32_t right,
                  std::uint32_t &value) noexcept {
  const auto wide = static_cast<std::int64_t>(asSigned(left)) +
                    static_cast<std::int64_t>(asSigned(right));
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    return true;
  }
  value = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
  return false;
}

bool subtractOverflows(std::uint32_t left, std::uint32_t right,
                       std::uint32_t &value) noexcept {
  const auto wide = static_cast<std::int64_t>(asSigned(left)) -
                    static_cast<std::int64_t>(asSigned(right));
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    return true;
  }
  value = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
  return false;
}

constexpr std::uint32_t registerBit(std::uint8_t reg) noexcept {
  return reg == 0U ? 0U : std::uint32_t{1U} << reg;
}

} // namespace

R3000CachedInstruction
R3000Runtime::decodeCachedInstruction(std::uint32_t instruction) noexcept {
  const auto opcode = static_cast<std::uint8_t>(instruction >> 26U);
  const auto rs = static_cast<std::uint8_t>((instruction >> 21U) & 31U);
  const auto rt = static_cast<std::uint8_t>((instruction >> 16U) & 31U);
  const auto function = static_cast<std::uint8_t>(instruction & 63U);

  auto operation = FastOperation::none;
  std::uint32_t source_mask{};
  const auto set_alu = [&](FastOperation candidate) noexcept {
    operation = candidate;
    source_mask = registerBit(rs) | registerBit(rt);
  };
  const auto set_immediate = [&](FastOperation candidate) noexcept {
    operation = candidate;
    source_mask = registerBit(rs);
  };

  if (opcode == 0U) {
    switch (function) {
    case 0x00U:
      set_alu(FastOperation::sll);
      break;
    case 0x02U:
      set_alu(FastOperation::srl);
      break;
    case 0x03U:
      set_alu(FastOperation::sra);
      break;
    case 0x04U:
      set_alu(FastOperation::sllv);
      break;
    case 0x06U:
      set_alu(FastOperation::srlv);
      break;
    case 0x07U:
      set_alu(FastOperation::srav);
      break;
    case 0x08U:
      operation = FastOperation::jr;
      break;
    case 0x09U:
      operation = FastOperation::jalr;
      break;
    case 0x10U:
      operation = FastOperation::mfhi;
      break;
    case 0x11U:
      operation = FastOperation::mthi;
      break;
    case 0x12U:
      operation = FastOperation::mflo;
      break;
    case 0x13U:
      operation = FastOperation::mtlo;
      break;
    case 0x18U:
      operation = FastOperation::mult;
      break;
    case 0x19U:
      operation = FastOperation::multu;
      break;
    case 0x1aU:
      operation = FastOperation::div;
      break;
    case 0x1bU:
      operation = FastOperation::divu;
      break;
    case 0x20U:
      set_alu(FastOperation::add);
      break;
    case 0x21U:
      set_alu(FastOperation::addu);
      break;
    case 0x22U:
      set_alu(FastOperation::sub);
      break;
    case 0x23U:
      set_alu(FastOperation::subu);
      break;
    case 0x24U:
      set_alu(FastOperation::and_);
      break;
    case 0x25U:
      set_alu(FastOperation::or_);
      break;
    case 0x26U:
      set_alu(FastOperation::xor_);
      break;
    case 0x27U:
      set_alu(FastOperation::nor);
      break;
    case 0x2aU:
      set_alu(FastOperation::slt);
      break;
    case 0x2bU:
      set_alu(FastOperation::sltu);
      break;
    default:
      break;
    }
  } else {
    switch (opcode) {
    case 0x01U:
      switch (rt) {
      case 0x00U:
        operation = FastOperation::bltz;
        break;
      case 0x01U:
        operation = FastOperation::bgez;
        break;
      case 0x10U:
        operation = FastOperation::bltzal;
        break;
      case 0x11U:
        operation = FastOperation::bgezal;
        break;
      default:
        break;
      }
      break;
    case 0x02U:
      operation = FastOperation::j;
      break;
    case 0x03U:
      operation = FastOperation::jal;
      break;
    case 0x04U:
      operation = FastOperation::beq;
      break;
    case 0x05U:
      operation = FastOperation::bne;
      break;
    case 0x06U:
      operation = FastOperation::blez;
      break;
    case 0x07U:
      operation = FastOperation::bgtz;
      break;
    case 0x08U:
      set_immediate(FastOperation::addi);
      break;
    case 0x09U:
      set_immediate(FastOperation::addiu);
      break;
    case 0x0aU:
      set_immediate(FastOperation::slti);
      break;
    case 0x0bU:
      set_immediate(FastOperation::sltiu);
      break;
    case 0x0cU:
      set_immediate(FastOperation::andi);
      break;
    case 0x0dU:
      set_immediate(FastOperation::ori);
      break;
    case 0x0eU:
      set_immediate(FastOperation::xori);
      break;
    case 0x0fU:
      operation = FastOperation::lui;
      break;
    case 0x20U:
      operation = FastOperation::lb;
      break;
    case 0x21U:
      operation = FastOperation::lh;
      break;
    case 0x23U:
      operation = FastOperation::lw;
      break;
    case 0x24U:
      operation = FastOperation::lbu;
      break;
    case 0x25U:
      operation = FastOperation::lhu;
      break;
    case 0x28U:
      operation = FastOperation::sb;
      break;
    case 0x29U:
      operation = FastOperation::sh;
      break;
    case 0x2bU:
      operation = FastOperation::sw;
      break;
    default:
      break;
    }
  }

  const auto synchronization_boundary =
      operation == FastOperation::none || memoryOperation(operation);
  const auto carrier_mask_required = source_mask != 0U ||
                                     writesRegister(operation) ||
                                     operation == FastOperation::sw;
  return {
      .raw = instruction,
      .source_mask = source_mask,
      .operation = static_cast<std::uint8_t>(operation),
      .synchronization_boundary = synchronization_boundary,
      .carrier_mask_required = carrier_mask_required,
  };
}

bool R3000Runtime::stepCachedFast(const R3000CachedInstruction &cached,
                                  R3000RunResult &result) noexcept {
  const auto instruction = cached.raw;
  const auto instruction_pc = state_.pc;
  if ((instruction_pc & 3U) != 0U || interruptPending()) {
    return false;
  }

  const auto operation = static_cast<FastOperation>(cached.operation);
  if (operation == FastOperation::none) {
    return false;
  }
  if (operation >= FastOperation::mfhi && operation <= FastOperation::divu &&
      pgxp_transform_tracking_ && pgxp_cpu_tracking_) {
    return false;
  }

  const auto rs = static_cast<std::uint8_t>((instruction >> 21U) & 31U);
  const auto rt = static_cast<std::uint8_t>((instruction >> 16U) & 31U);
  const auto rd = static_cast<std::uint8_t>((instruction >> 11U) & 31U);
  const auto shift = static_cast<std::uint8_t>((instruction >> 6U) & 31U);
  const auto immediate = instruction & 0xffffU;
  const auto left = state_.gpr[rs];
  const auto right = state_.gpr[rt];

  std::uint32_t carrier_mask{};
  if (cached.carrier_mask_required) {
    carrier_mask = gpu_projection_handle_gpr_mask_ | projected_gpr_valid_mask_ |
                   projected_half_gpr_valid_mask_;
    if (exact_tracking_ != nullptr) {
      carrier_mask |= exact_tracking_->gpr_valid_mask;
      if (pgxp_cpu_tracking_) {
        carrier_mask |=
            static_cast<std::uint32_t>(exact_tracking_->pgxp_gpr_valid_mask);
      }
    }
  }

  const auto source_mask = cached.source_mask;
  const auto tracked_source = (carrier_mask & source_mask) != 0U;
  const auto tracked_add_immediate =
      tracked_source &&
      (operation == FastOperation::addi || operation == FastOperation::addiu);
  if (tracked_source && !tracked_add_immediate) {
    return false;
  }

  const auto memory_operation = memoryOperation(operation);
  std::uint32_t memory_address{};
  if (memory_operation) {
    memory_address = left + signExtend16(immediate);
    std::uint32_t physical{};
    if (!physicalAddress(memory_address, physical) || physical >= 0x00800000U) {
      return false;
    }
  }

  std::uint32_t arithmetic_value{};
  if (operation == FastOperation::add &&
      addOverflows(left, right, arithmetic_value)) {
    return false;
  }
  if (operation == FastOperation::sub &&
      subtractOverflows(left, right, arithmetic_value)) {
    return false;
  }
  if (operation == FastOperation::addi &&
      addOverflows(left, signExtend16(immediate), arithmetic_value)) {
    return false;
  }

  state_.branch_delay_slot = false;
  state_.pc = state_.next_pc;
  state_.next_pc += 4U;

  auto stop = R3000StopReason::running;
  auto write = [&](std::uint8_t reg, std::uint32_t value) noexcept {
    if (reg != 0U) {
      const auto register_bit = registerBit(reg);
      if ((carrier_mask & register_bit) != 0U ||
          (state_.load_delay.valid && state_.load_delay.reg == reg)) {
        writeRegister(reg, value);
      } else {
        state_.gpr[reg] = value;
      }
    }
  };
  auto branch = [&](bool taken) noexcept {
    state_.branch_pc = instruction_pc;
    state_.branch_delay_slot = true;
    if (taken) {
      state_.next_pc = instruction_pc + 4U + (signExtend16(immediate) << 2U);
    }
  };

  switch (operation) {
  case FastOperation::sll:
    write(rd, right << shift);
    break;
  case FastOperation::srl:
    write(rd, right >> shift);
    break;
  case FastOperation::sra:
    write(rd, arithmeticShiftRight(right, shift));
    break;
  case FastOperation::sllv:
    write(rd, right << (left & 31U));
    break;
  case FastOperation::srlv:
    write(rd, right >> (left & 31U));
    break;
  case FastOperation::srav:
    write(rd, arithmeticShiftRight(right, left));
    break;
  case FastOperation::jr:
    state_.branch_pc = instruction_pc;
    state_.branch_delay_slot = true;
    state_.next_pc = left;
    break;
  case FastOperation::jalr:
    write(rd, instruction_pc + 8U);
    state_.branch_pc = instruction_pc;
    state_.branch_delay_slot = true;
    state_.next_pc = left;
    break;
  case FastOperation::mfhi:
    write(rd, state_.hi);
    break;
  case FastOperation::mthi:
    state_.hi = left;
    break;
  case FastOperation::mflo:
    write(rd, state_.lo);
    break;
  case FastOperation::mtlo:
    state_.lo = left;
    break;
  case FastOperation::mult: {
    const auto product = static_cast<std::int64_t>(asSigned(left)) *
                         static_cast<std::int64_t>(asSigned(right));
    const auto bits = static_cast<std::uint64_t>(product);
    state_.lo = static_cast<std::uint32_t>(bits);
    state_.hi = static_cast<std::uint32_t>(bits >> 32U);
    break;
  }
  case FastOperation::multu: {
    const auto product = static_cast<std::uint64_t>(left) * right;
    state_.lo = static_cast<std::uint32_t>(product);
    state_.hi = static_cast<std::uint32_t>(product >> 32U);
    break;
  }
  case FastOperation::div: {
    const auto numerator = asSigned(left);
    const auto denominator = asSigned(right);
    if (denominator == 0) {
      state_.lo = numerator >= 0 ? 0xffffffffU : 1U;
      state_.hi = left;
    } else if (left == 0x80000000U && denominator == -1) {
      state_.lo = 0x80000000U;
      state_.hi = 0U;
    } else {
      state_.lo = static_cast<std::uint32_t>(numerator / denominator);
      state_.hi = static_cast<std::uint32_t>(numerator % denominator);
    }
    break;
  }
  case FastOperation::divu:
    if (right == 0U) {
      state_.lo = 0xffffffffU;
      state_.hi = left;
    } else {
      state_.lo = left / right;
      state_.hi = left % right;
    }
    break;
  case FastOperation::add:
    write(rd, arithmetic_value);
    break;
  case FastOperation::addi:
    if (tracked_add_immediate) {
      writeAddImmediate(rs, rt, left, signExtend16(immediate), arithmetic_value,
                        false);
    } else {
      write(rt, arithmetic_value);
    }
    break;
  case FastOperation::addu:
    write(rd, left + right);
    break;
  case FastOperation::sub:
    write(rd, arithmetic_value);
    break;
  case FastOperation::subu:
    write(rd, left - right);
    break;
  case FastOperation::and_:
    write(rd, left & right);
    break;
  case FastOperation::or_:
    write(rd, left | right);
    break;
  case FastOperation::xor_:
    write(rd, left ^ right);
    break;
  case FastOperation::nor:
    write(rd, ~(left | right));
    break;
  case FastOperation::slt:
    write(rd, asSigned(left) < asSigned(right) ? 1U : 0U);
    break;
  case FastOperation::sltu:
    write(rd, left < right ? 1U : 0U);
    break;
  case FastOperation::bltz:
    branch(asSigned(left) < 0);
    break;
  case FastOperation::bgez:
    branch(asSigned(left) >= 0);
    break;
  case FastOperation::bltzal:
    write(31U, instruction_pc + 8U);
    branch(asSigned(left) < 0);
    break;
  case FastOperation::bgezal:
    write(31U, instruction_pc + 8U);
    branch(asSigned(left) >= 0);
    break;
  case FastOperation::j:
  case FastOperation::jal:
    if (operation == FastOperation::jal) {
      write(31U, instruction_pc + 8U);
    }
    state_.branch_pc = instruction_pc;
    state_.branch_delay_slot = true;
    state_.next_pc =
        (state_.pc & 0xf0000000U) | ((instruction & 0x03ffffffU) << 2U);
    break;
  case FastOperation::beq:
    branch(left == right);
    break;
  case FastOperation::bne:
    branch(left != right);
    break;
  case FastOperation::blez:
    branch(asSigned(left) <= 0);
    break;
  case FastOperation::bgtz:
    branch(asSigned(left) > 0);
    break;
  case FastOperation::addiu:
    if (tracked_add_immediate) {
      const auto immediate_value = signExtend16(immediate);
      writeAddImmediate(rs, rt, left, immediate_value, left + immediate_value,
                        true);
    } else {
      write(rt, left + signExtend16(immediate));
    }
    break;
  case FastOperation::slti:
    write(rt, asSigned(left) < asSigned(signExtend16(immediate)) ? 1U : 0U);
    break;
  case FastOperation::sltiu:
    write(rt, left < signExtend16(immediate) ? 1U : 0U);
    break;
  case FastOperation::andi:
    write(rt, left & immediate);
    break;
  case FastOperation::ori:
    write(rt, left | immediate);
    break;
  case FastOperation::xori:
    write(rt, left ^ immediate);
    break;
  case FastOperation::lui:
    write(rt, immediate << 16U);
    break;
  case FastOperation::lb:
  case FastOperation::lbu: {
    std::uint8_t value{};
    if (!read8(memory_address, value)) {
      stop = R3000StopReason::memory_fault;
    } else if (operation == FastOperation::lb) {
      scheduleLoad(rt, static_cast<std::uint32_t>(static_cast<std::int32_t>(
                           static_cast<std::int8_t>(value))));
    } else {
      scheduleLoad(rt, value);
    }
    break;
  }
  case FastOperation::lh:
  case FastOperation::lhu:
    stop = loadHalfword(memory_address, rt, operation == FastOperation::lh);
    break;
  case FastOperation::lw: {
    if ((memory_address & 3U) != 0U) {
      stop = R3000StopReason::alignment_fault;
      break;
    }
    std::uint32_t value{};
    if (!read32(memory_address, value)) {
      stop = R3000StopReason::memory_fault;
      break;
    }
    const auto *projected =
        projectedVertexProvenanceAt(memory_address, value).projected;
    scheduleLoad(rt, value, projected, nullptr,
                 exactWordAt(memory_address, value),
                 pgxpWordAt(memory_address, value));
    break;
  }
  case FastOperation::sb:
    if (!write8(memory_address, static_cast<std::uint8_t>(right))) {
      stop = R3000StopReason::memory_fault;
    }
    break;
  case FastOperation::sh:
    stop = storeHalfword(memory_address, rt, right, instruction_pc);
    break;
  case FastOperation::sw: {
    if ((memory_address & 3U) != 0U) {
      stop = R3000StopReason::alignment_fault;
      break;
    }
    const auto source_bit = registerBit(rt);
    const auto *projected = (carrier_mask & source_bit) != 0U
                                ? projectedRegister(rt, right)
                                : nullptr;
    const auto *exact = exactRegister(rt, right);
    if (!write32Projected(memory_address, right, projected, instruction_pc)) {
      stop = R3000StopReason::memory_fault;
      break;
    }
    if (projected != nullptr) {
      storeProjectedVertex(memory_address, *projected);
    }
    if (exact != nullptr) {
      storeExactWord(memory_address, right, exact);
    }
    if (projected == nullptr) {
      storePgxpWord(memory_address, right, pgxpRegister(rt, right));
    }
    break;
  }
  case FastOperation::none:
    return false;
  }

  if (stop != R3000StopReason::running) {
    state_.next_load_delay = {};
    gpu_projection_handle_next_load_delay_ = 0U;
    projected_next_load_delay_ = {};
    if (pgxp_transform_tracking_) {
      projected_half_next_load_delay_ = {};
      exact_tracking_->next_load_delay = {};
    }
    advanceLoadDelay();
    result = {stop, 0U, instruction_pc, instruction};
    return true;
  }

  advanceLoadDelay();
  result = {R3000StopReason::running, 1U, instruction_pc, instruction};
  return true;
}

} // namespace sf::psx
