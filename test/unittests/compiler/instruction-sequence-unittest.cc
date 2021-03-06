// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/utils/random-number-generator.h"
#include "src/compiler/pipeline.h"
#include "test/unittests/compiler/instruction-sequence-unittest.h"
#include "test/unittests/test-utils.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace v8 {
namespace internal {
namespace compiler {

static const char*
    general_register_names_[RegisterConfiguration::kMaxGeneralRegisters];
static const char*
    double_register_names_[RegisterConfiguration::kMaxDoubleRegisters];
static char register_names_[10 * (RegisterConfiguration::kMaxGeneralRegisters +
                                  RegisterConfiguration::kMaxDoubleRegisters)];


static void InitializeRegisterNames() {
  char* loc = register_names_;
  for (int i = 0; i < RegisterConfiguration::kMaxGeneralRegisters; ++i) {
    general_register_names_[i] = loc;
    loc += base::OS::SNPrintF(loc, 100, "gp_%d", i);
    *loc++ = 0;
  }
  for (int i = 0; i < RegisterConfiguration::kMaxDoubleRegisters; ++i) {
    double_register_names_[i] = loc;
    loc += base::OS::SNPrintF(loc, 100, "fp_%d", i) + 1;
    *loc++ = 0;
  }
}


InstructionSequenceTest::InstructionSequenceTest()
    : sequence_(nullptr),
      num_general_registers_(kDefaultNRegs),
      num_double_registers_(kDefaultNRegs),
      instruction_blocks_(zone()),
      current_block_(nullptr),
      block_returns_(false) {
  InitializeRegisterNames();
}


void InstructionSequenceTest::SetNumRegs(int num_general_registers,
                                         int num_double_registers) {
  CHECK(config_.is_empty());
  CHECK(instructions_.empty());
  CHECK(instruction_blocks_.empty());
  num_general_registers_ = num_general_registers;
  num_double_registers_ = num_double_registers;
}


RegisterConfiguration* InstructionSequenceTest::config() {
  if (config_.is_empty()) {
    config_.Reset(new RegisterConfiguration(
        num_general_registers_, num_double_registers_, num_double_registers_,
        general_register_names_, double_register_names_));
  }
  return config_.get();
}


InstructionSequence* InstructionSequenceTest::sequence() {
  if (sequence_ == nullptr) {
    sequence_ = new (zone())
        InstructionSequence(isolate(), zone(), &instruction_blocks_);
  }
  return sequence_;
}


void InstructionSequenceTest::StartLoop(int loop_blocks) {
  CHECK(current_block_ == nullptr);
  if (!loop_blocks_.empty()) {
    CHECK(!loop_blocks_.back().loop_header_.IsValid());
  }
  LoopData loop_data = {Rpo::Invalid(), loop_blocks};
  loop_blocks_.push_back(loop_data);
}


void InstructionSequenceTest::EndLoop() {
  CHECK(current_block_ == nullptr);
  CHECK(!loop_blocks_.empty());
  CHECK_EQ(0, loop_blocks_.back().expected_blocks_);
  loop_blocks_.pop_back();
}


void InstructionSequenceTest::StartBlock() {
  block_returns_ = false;
  NewBlock();
}


Instruction* InstructionSequenceTest::EndBlock(BlockCompletion completion) {
  Instruction* result = nullptr;
  if (block_returns_) {
    CHECK(completion.type_ == kBlockEnd || completion.type_ == kFallThrough);
    completion.type_ = kBlockEnd;
  }
  switch (completion.type_) {
    case kBlockEnd:
      break;
    case kFallThrough:
      result = EmitFallThrough();
      break;
    case kJump:
      CHECK(!block_returns_);
      result = EmitJump();
      break;
    case kBranch:
      CHECK(!block_returns_);
      result = EmitBranch(completion.op_);
      break;
  }
  completions_.push_back(completion);
  CHECK(current_block_ != nullptr);
  sequence()->EndBlock(current_block_->rpo_number());
  current_block_ = nullptr;
  return result;
}


InstructionSequenceTest::TestOperand InstructionSequenceTest::Imm(int32_t imm) {
  int index = sequence()->AddImmediate(Constant(imm));
  return TestOperand(kImmediate, index);
}


InstructionSequenceTest::VReg InstructionSequenceTest::Define(
    TestOperand output_op) {
  VReg vreg = NewReg();
  InstructionOperand outputs[1]{ConvertOutputOp(vreg, output_op)};
  Emit(kArchNop, 1, outputs);
  return vreg;
}


Instruction* InstructionSequenceTest::Return(TestOperand input_op_0) {
  block_returns_ = true;
  InstructionOperand inputs[1]{ConvertInputOp(input_op_0)};
  return Emit(kArchRet, 0, nullptr, 1, inputs);
}


PhiInstruction* InstructionSequenceTest::Phi(VReg incoming_vreg_0,
                                             VReg incoming_vreg_1,
                                             VReg incoming_vreg_2,
                                             VReg incoming_vreg_3) {
  VReg inputs[] = {incoming_vreg_0, incoming_vreg_1, incoming_vreg_2,
                   incoming_vreg_3};
  size_t input_count = 0;
  for (; input_count < arraysize(inputs); ++input_count) {
    if (inputs[input_count].value_ == kNoValue) break;
  }
  CHECK(input_count > 0);
  auto phi = new (zone()) PhiInstruction(zone(), NewReg().value_, input_count);
  for (size_t i = 0; i < input_count; ++i) {
    SetInput(phi, i, inputs[i]);
  }
  current_block_->AddPhi(phi);
  return phi;
}


PhiInstruction* InstructionSequenceTest::Phi(VReg incoming_vreg_0,
                                             size_t input_count) {
  auto phi = new (zone()) PhiInstruction(zone(), NewReg().value_, input_count);
  SetInput(phi, 0, incoming_vreg_0);
  current_block_->AddPhi(phi);
  return phi;
}


void InstructionSequenceTest::SetInput(PhiInstruction* phi, size_t input,
                                       VReg vreg) {
  CHECK(vreg.value_ != kNoValue);
  phi->SetInput(input, vreg.value_);
}


InstructionSequenceTest::VReg InstructionSequenceTest::DefineConstant(
    int32_t imm) {
  VReg vreg = NewReg();
  sequence()->AddConstant(vreg.value_, Constant(imm));
  InstructionOperand outputs[1]{ConstantOperand(vreg.value_)};
  Emit(kArchNop, 1, outputs);
  return vreg;
}


Instruction* InstructionSequenceTest::EmitNop() { return Emit(kArchNop); }


static size_t CountInputs(size_t size,
                          InstructionSequenceTest::TestOperand* inputs) {
  size_t i = 0;
  for (; i < size; ++i) {
    if (inputs[i].type_ == InstructionSequenceTest::kInvalid) break;
  }
  return i;
}


Instruction* InstructionSequenceTest::EmitI(size_t input_size,
                                            TestOperand* inputs) {
  InstructionOperand* mapped_inputs = ConvertInputs(input_size, inputs);
  return Emit(kArchNop, 0, nullptr, input_size, mapped_inputs);
}


Instruction* InstructionSequenceTest::EmitI(TestOperand input_op_0,
                                            TestOperand input_op_1,
                                            TestOperand input_op_2,
                                            TestOperand input_op_3) {
  TestOperand inputs[] = {input_op_0, input_op_1, input_op_2, input_op_3};
  return EmitI(CountInputs(arraysize(inputs), inputs), inputs);
}


InstructionSequenceTest::VReg InstructionSequenceTest::EmitOI(
    TestOperand output_op, size_t input_size, TestOperand* inputs) {
  VReg output_vreg = NewReg();
  InstructionOperand outputs[1]{ConvertOutputOp(output_vreg, output_op)};
  InstructionOperand* mapped_inputs = ConvertInputs(input_size, inputs);
  Emit(kArchNop, 1, outputs, input_size, mapped_inputs);
  return output_vreg;
}


InstructionSequenceTest::VReg InstructionSequenceTest::EmitOI(
    TestOperand output_op, TestOperand input_op_0, TestOperand input_op_1,
    TestOperand input_op_2, TestOperand input_op_3) {
  TestOperand inputs[] = {input_op_0, input_op_1, input_op_2, input_op_3};
  return EmitOI(output_op, CountInputs(arraysize(inputs), inputs), inputs);
}


InstructionSequenceTest::VRegPair InstructionSequenceTest::EmitOOI(
    TestOperand output_op_0, TestOperand output_op_1, size_t input_size,
    TestOperand* inputs) {
  VRegPair output_vregs = std::make_pair(NewReg(), NewReg());
  InstructionOperand outputs[2]{
      ConvertOutputOp(output_vregs.first, output_op_0),
      ConvertOutputOp(output_vregs.second, output_op_1)};
  InstructionOperand* mapped_inputs = ConvertInputs(input_size, inputs);
  Emit(kArchNop, 2, outputs, input_size, mapped_inputs);
  return output_vregs;
}


InstructionSequenceTest::VRegPair InstructionSequenceTest::EmitOOI(
    TestOperand output_op_0, TestOperand output_op_1, TestOperand input_op_0,
    TestOperand input_op_1, TestOperand input_op_2, TestOperand input_op_3) {
  TestOperand inputs[] = {input_op_0, input_op_1, input_op_2, input_op_3};
  return EmitOOI(output_op_0, output_op_1,
                 CountInputs(arraysize(inputs), inputs), inputs);
}


InstructionSequenceTest::VReg InstructionSequenceTest::EmitCall(
    TestOperand output_op, size_t input_size, TestOperand* inputs) {
  VReg output_vreg = NewReg();
  InstructionOperand outputs[1]{ConvertOutputOp(output_vreg, output_op)};
  CHECK(UnallocatedOperand::cast(outputs[0]).HasFixedPolicy());
  InstructionOperand* mapped_inputs = ConvertInputs(input_size, inputs);
  Emit(kArchCallCodeObject, 1, outputs, input_size, mapped_inputs, 0, nullptr,
       true);
  return output_vreg;
}


InstructionSequenceTest::VReg InstructionSequenceTest::EmitCall(
    TestOperand output_op, TestOperand input_op_0, TestOperand input_op_1,
    TestOperand input_op_2, TestOperand input_op_3) {
  TestOperand inputs[] = {input_op_0, input_op_1, input_op_2, input_op_3};
  return EmitCall(output_op, CountInputs(arraysize(inputs), inputs), inputs);
}


Instruction* InstructionSequenceTest::EmitBranch(TestOperand input_op) {
  InstructionOperand inputs[4]{ConvertInputOp(input_op), ConvertInputOp(Imm()),
                               ConvertInputOp(Imm()), ConvertInputOp(Imm())};
  InstructionCode opcode = kArchJmp | FlagsModeField::encode(kFlags_branch) |
                           FlagsConditionField::encode(kEqual);
  auto instruction = NewInstruction(opcode, 0, nullptr, 4, inputs);
  return AddInstruction(instruction);
}


Instruction* InstructionSequenceTest::EmitFallThrough() {
  auto instruction = NewInstruction(kArchNop, 0, nullptr);
  return AddInstruction(instruction);
}


Instruction* InstructionSequenceTest::EmitJump() {
  InstructionOperand inputs[1]{ConvertInputOp(Imm())};
  auto instruction = NewInstruction(kArchJmp, 0, nullptr, 1, inputs);
  return AddInstruction(instruction);
}


Instruction* InstructionSequenceTest::NewInstruction(
    InstructionCode code, size_t outputs_size, InstructionOperand* outputs,
    size_t inputs_size, InstructionOperand* inputs, size_t temps_size,
    InstructionOperand* temps) {
  CHECK(current_block_);
  return Instruction::New(zone(), code, outputs_size, outputs, inputs_size,
                          inputs, temps_size, temps);
}


InstructionOperand InstructionSequenceTest::Unallocated(
    TestOperand op, UnallocatedOperand::ExtendedPolicy policy) {
  return UnallocatedOperand(policy, op.vreg_.value_);
}


InstructionOperand InstructionSequenceTest::Unallocated(
    TestOperand op, UnallocatedOperand::ExtendedPolicy policy,
    UnallocatedOperand::Lifetime lifetime) {
  return UnallocatedOperand(policy, lifetime, op.vreg_.value_);
}


InstructionOperand InstructionSequenceTest::Unallocated(
    TestOperand op, UnallocatedOperand::ExtendedPolicy policy, int index) {
  return UnallocatedOperand(policy, index, op.vreg_.value_);
}


InstructionOperand InstructionSequenceTest::Unallocated(
    TestOperand op, UnallocatedOperand::BasicPolicy policy, int index) {
  return UnallocatedOperand(policy, index, op.vreg_.value_);
}


InstructionOperand* InstructionSequenceTest::ConvertInputs(
    size_t input_size, TestOperand* inputs) {
  InstructionOperand* mapped_inputs =
      zone()->NewArray<InstructionOperand>(static_cast<int>(input_size));
  for (size_t i = 0; i < input_size; ++i) {
    mapped_inputs[i] = ConvertInputOp(inputs[i]);
  }
  return mapped_inputs;
}


InstructionOperand InstructionSequenceTest::ConvertInputOp(TestOperand op) {
  if (op.type_ == kImmediate) {
    CHECK_EQ(op.vreg_.value_, kNoValue);
    return ImmediateOperand(op.value_);
  }
  CHECK_NE(op.vreg_.value_, kNoValue);
  switch (op.type_) {
    case kNone:
      return Unallocated(op, UnallocatedOperand::NONE,
                         UnallocatedOperand::USED_AT_START);
    case kUnique:
      return Unallocated(op, UnallocatedOperand::NONE);
    case kUniqueRegister:
      return Unallocated(op, UnallocatedOperand::MUST_HAVE_REGISTER);
    case kRegister:
      return Unallocated(op, UnallocatedOperand::MUST_HAVE_REGISTER,
                         UnallocatedOperand::USED_AT_START);
    case kSlot:
      return Unallocated(op, UnallocatedOperand::MUST_HAVE_SLOT,
                         UnallocatedOperand::USED_AT_START);
    case kFixedRegister:
      CHECK(0 <= op.value_ && op.value_ < num_general_registers_);
      return Unallocated(op, UnallocatedOperand::FIXED_REGISTER, op.value_);
    case kFixedSlot:
      return Unallocated(op, UnallocatedOperand::FIXED_SLOT, op.value_);
    default:
      break;
  }
  CHECK(false);
  return InstructionOperand();
}


InstructionOperand InstructionSequenceTest::ConvertOutputOp(VReg vreg,
                                                            TestOperand op) {
  CHECK_EQ(op.vreg_.value_, kNoValue);
  op.vreg_ = vreg;
  switch (op.type_) {
    case kSameAsFirst:
      return Unallocated(op, UnallocatedOperand::SAME_AS_FIRST_INPUT);
    case kRegister:
      return Unallocated(op, UnallocatedOperand::MUST_HAVE_REGISTER);
    case kFixedSlot:
      return Unallocated(op, UnallocatedOperand::FIXED_SLOT, op.value_);
    case kFixedRegister:
      CHECK(0 <= op.value_ && op.value_ < num_general_registers_);
      return Unallocated(op, UnallocatedOperand::FIXED_REGISTER, op.value_);
    default:
      break;
  }
  CHECK(false);
  return InstructionOperand();
}


InstructionBlock* InstructionSequenceTest::NewBlock() {
  CHECK(current_block_ == nullptr);
  Rpo rpo = Rpo::FromInt(static_cast<int>(instruction_blocks_.size()));
  Rpo loop_header = Rpo::Invalid();
  Rpo loop_end = Rpo::Invalid();
  if (!loop_blocks_.empty()) {
    auto& loop_data = loop_blocks_.back();
    // This is a loop header.
    if (!loop_data.loop_header_.IsValid()) {
      loop_end = Rpo::FromInt(rpo.ToInt() + loop_data.expected_blocks_);
      loop_data.expected_blocks_--;
      loop_data.loop_header_ = rpo;
    } else {
      // This is a loop body.
      CHECK_NE(0, loop_data.expected_blocks_);
      // TODO(dcarney): handle nested loops.
      loop_data.expected_blocks_--;
      loop_header = loop_data.loop_header_;
    }
  }
  // Construct instruction block.
  auto instruction_block =
      new (zone()) InstructionBlock(zone(), rpo, loop_header, loop_end, false);
  instruction_blocks_.push_back(instruction_block);
  current_block_ = instruction_block;
  sequence()->StartBlock(rpo);
  return instruction_block;
}


void InstructionSequenceTest::WireBlocks() {
  CHECK(!current_block());
  CHECK(instruction_blocks_.size() == completions_.size());
  size_t offset = 0;
  for (const auto& completion : completions_) {
    switch (completion.type_) {
      case kBlockEnd:
        break;
      case kFallThrough:  // Fallthrough.
      case kJump:
        WireBlock(offset, completion.offset_0_);
        break;
      case kBranch:
        WireBlock(offset, completion.offset_0_);
        WireBlock(offset, completion.offset_1_);
        break;
    }
    ++offset;
  }
}


void InstructionSequenceTest::WireBlock(size_t block_offset, int jump_offset) {
  size_t target_block_offset = block_offset + static_cast<size_t>(jump_offset);
  CHECK(block_offset < instruction_blocks_.size());
  CHECK(target_block_offset < instruction_blocks_.size());
  auto block = instruction_blocks_[block_offset];
  auto target = instruction_blocks_[target_block_offset];
  block->successors().push_back(target->rpo_number());
  target->predecessors().push_back(block->rpo_number());
}


Instruction* InstructionSequenceTest::Emit(
    InstructionCode code, size_t outputs_size, InstructionOperand* outputs,
    size_t inputs_size, InstructionOperand* inputs, size_t temps_size,
    InstructionOperand* temps, bool is_call) {
  auto instruction = NewInstruction(code, outputs_size, outputs, inputs_size,
                                    inputs, temps_size, temps);
  if (is_call) instruction->MarkAsCall();
  return AddInstruction(instruction);
}


Instruction* InstructionSequenceTest::AddInstruction(Instruction* instruction) {
  sequence()->AddInstruction(instruction);
  return instruction;
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
