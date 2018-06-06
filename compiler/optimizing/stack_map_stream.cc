/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "stack_map_stream.h"

#include <memory>

#include "art_method-inl.h"
#include "base/stl_util.h"
#include "dex/dex_file_types.h"
#include "optimizing/optimizing_compiler.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "stack_map.h"

namespace art {

constexpr static bool kVerifyStackMaps = kIsDebugBuild;

uint32_t StackMapStream::GetStackMapNativePcOffset(size_t i) {
  return StackMap::UnpackNativePc(stack_maps_[i].packed_native_pc, instruction_set_);
}

void StackMapStream::SetStackMapNativePcOffset(size_t i, uint32_t native_pc_offset) {
  stack_maps_[i].packed_native_pc = StackMap::PackNativePc(native_pc_offset, instruction_set_);
}

void StackMapStream::BeginStackMapEntry(uint32_t dex_pc,
                                        uint32_t native_pc_offset,
                                        uint32_t register_mask,
                                        BitVector* stack_mask,
                                        uint32_t num_dex_registers,
                                        uint8_t inlining_depth) {
  DCHECK(!in_stack_map_) << "Mismatched Begin/End calls";
  in_stack_map_ = true;

  current_stack_map_ = StackMapEntry {
    .packed_native_pc = StackMap::PackNativePc(native_pc_offset, instruction_set_),
    .dex_pc = dex_pc,
    .register_mask_index = kNoValue,
    .stack_mask_index = kNoValue,
    .inline_info_index = kNoValue,
    .dex_register_mask_index = kNoValue,
    .dex_register_map_index = kNoValue,
  };
  if (register_mask != 0) {
    uint32_t shift = LeastSignificantBit(register_mask);
    RegisterMaskEntry entry = { register_mask >> shift, shift };
    current_stack_map_.register_mask_index = register_masks_.Dedup(&entry);
  }
  // The compiler assumes the bit vector will be read during PrepareForFillIn(),
  // and it might modify the data before that. Therefore, just store the pointer.
  // See ClearSpillSlotsFromLoopPhisInStackMap in code_generator.h.
  lazy_stack_masks_.push_back(stack_mask);
  current_inline_infos_.clear();
  current_dex_registers_.clear();
  expected_num_dex_registers_ = num_dex_registers;

  if (kVerifyStackMaps) {
    size_t stack_map_index = stack_maps_.size();
    // Create lambda method, which will be executed at the very end to verify data.
    // Parameters and local variables will be captured(stored) by the lambda "[=]".
    dchecks_.emplace_back([=](const CodeInfo& code_info) {
      StackMap stack_map = code_info.GetStackMapAt(stack_map_index);
      CHECK_EQ(stack_map.GetNativePcOffset(instruction_set_), native_pc_offset);
      CHECK_EQ(stack_map.GetDexPc(), dex_pc);
      CHECK_EQ(code_info.GetRegisterMaskOf(stack_map), register_mask);
      BitMemoryRegion seen_stack_mask = code_info.GetStackMaskOf(stack_map);
      CHECK_GE(seen_stack_mask.size_in_bits(), stack_mask ? stack_mask->GetNumberOfBits() : 0);
      for (size_t b = 0; b < seen_stack_mask.size_in_bits(); b++) {
        CHECK_EQ(seen_stack_mask.LoadBit(b), stack_mask != nullptr && stack_mask->IsBitSet(b));
      }
      CHECK_EQ(stack_map.HasInlineInfo(), (inlining_depth != 0));
      CHECK_EQ(code_info.GetInlineDepthOf(stack_map), inlining_depth);
      CHECK_EQ(stack_map.HasDexRegisterMap(), (num_dex_registers != 0));
    });
  }
}

void StackMapStream::EndStackMapEntry() {
  DCHECK(in_stack_map_) << "Mismatched Begin/End calls";
  in_stack_map_ = false;
  DCHECK_EQ(expected_num_dex_registers_, current_dex_registers_.size());

  // Generate index into the InlineInfo table.
  if (!current_inline_infos_.empty()) {
    current_inline_infos_.back().is_last = InlineInfo::kLast;
    current_stack_map_.inline_info_index =
        inline_infos_.Dedup(current_inline_infos_.data(), current_inline_infos_.size());
  }

  stack_maps_.Add(current_stack_map_);
}

void StackMapStream::AddDexRegisterEntry(DexRegisterLocation::Kind kind, int32_t value) {
  current_dex_registers_.push_back(DexRegisterLocation(kind, value));

  // We have collected all the dex registers for StackMap/InlineInfo - create the map.
  if (current_dex_registers_.size() == expected_num_dex_registers_) {
    CreateDexRegisterMap();
  }
}

void StackMapStream::AddInvoke(InvokeType invoke_type, uint32_t dex_method_index) {
  uint32_t packed_native_pc = current_stack_map_.packed_native_pc;
  size_t invoke_info_index = invoke_infos_.size();
  invoke_infos_.Add(InvokeInfoEntry {
    .packed_native_pc = packed_native_pc,
    .invoke_type = invoke_type,
    .method_info_index = method_infos_.Dedup(&dex_method_index),
  });

  if (kVerifyStackMaps) {
    dchecks_.emplace_back([=](const CodeInfo& code_info) {
      InvokeInfo invoke_info = code_info.GetInvokeInfo(invoke_info_index);
      CHECK_EQ(invoke_info.GetNativePcOffset(instruction_set_),
               StackMap::UnpackNativePc(packed_native_pc, instruction_set_));
      CHECK_EQ(invoke_info.GetInvokeType(), invoke_type);
      CHECK_EQ(method_infos_[invoke_info.GetMethodIndexIdx()], dex_method_index);
    });
  }
}

void StackMapStream::BeginInlineInfoEntry(ArtMethod* method,
                                          uint32_t dex_pc,
                                          uint32_t num_dex_registers,
                                          const DexFile* outer_dex_file) {
  DCHECK(!in_inline_info_) << "Mismatched Begin/End calls";
  in_inline_info_ = true;
  DCHECK_EQ(expected_num_dex_registers_, current_dex_registers_.size());

  InlineInfoEntry entry = {
    .is_last = InlineInfo::kMore,
    .dex_pc = dex_pc,
    .method_info_index = kNoValue,
    .art_method_hi = kNoValue,
    .art_method_lo = kNoValue,
    .dex_register_mask_index = kNoValue,
    .dex_register_map_index = kNoValue,
  };
  if (EncodeArtMethodInInlineInfo(method)) {
    entry.art_method_hi = High32Bits(reinterpret_cast<uintptr_t>(method));
    entry.art_method_lo = Low32Bits(reinterpret_cast<uintptr_t>(method));
  } else {
    if (dex_pc != static_cast<uint32_t>(-1) && kIsDebugBuild) {
      ScopedObjectAccess soa(Thread::Current());
      DCHECK(IsSameDexFile(*outer_dex_file, *method->GetDexFile()));
    }
    uint32_t dex_method_index = method->GetDexMethodIndexUnchecked();
    entry.method_info_index = method_infos_.Dedup(&dex_method_index);
  }
  current_inline_infos_.push_back(entry);

  current_dex_registers_.clear();
  expected_num_dex_registers_ = num_dex_registers;

  if (kVerifyStackMaps) {
    size_t stack_map_index = stack_maps_.size();
    size_t depth = current_inline_infos_.size() - 1;
    dchecks_.emplace_back([=](const CodeInfo& code_info) {
      StackMap stack_map = code_info.GetStackMapAt(stack_map_index);
      InlineInfo inline_info = code_info.GetInlineInfoAtDepth(stack_map, depth);
      CHECK_EQ(inline_info.GetDexPc(), dex_pc);
      bool encode_art_method = EncodeArtMethodInInlineInfo(method);
      CHECK_EQ(inline_info.EncodesArtMethod(), encode_art_method);
      if (encode_art_method) {
        CHECK_EQ(inline_info.GetArtMethod(), method);
      } else {
        CHECK_EQ(method_infos_[inline_info.GetMethodIndexIdx()],
                 method->GetDexMethodIndexUnchecked());
      }
      CHECK_EQ(inline_info.HasDexRegisterMap(), (num_dex_registers != 0));
    });
  }
}

void StackMapStream::EndInlineInfoEntry() {
  DCHECK(in_inline_info_) << "Mismatched Begin/End calls";
  in_inline_info_ = false;
  DCHECK_EQ(expected_num_dex_registers_, current_dex_registers_.size());
}

// Create dex register map (bitmap + indices + catalogue entries)
// based on the currently accumulated list of DexRegisterLocations.
void StackMapStream::CreateDexRegisterMap() {
  // Create mask and map based on current registers.
  temp_dex_register_mask_.ClearAllBits();
  temp_dex_register_map_.clear();
  for (size_t i = 0; i < current_dex_registers_.size(); i++) {
    DexRegisterLocation reg = current_dex_registers_[i];
    if (reg.IsLive()) {
      DexRegisterEntry entry = DexRegisterEntry {
        .kind = static_cast<uint32_t>(reg.GetKind()),
        .packed_value = DexRegisterInfo::PackValue(reg.GetKind(), reg.GetValue()),
      };
      temp_dex_register_mask_.SetBit(i);
      temp_dex_register_map_.push_back(dex_register_catalog_.Dedup(&entry));
    }
  }

  // Set the mask and map for the current StackMap/InlineInfo.
  uint32_t mask_index = StackMap::kNoValue;  // Represents mask with all zero bits.
  if (temp_dex_register_mask_.GetNumberOfBits() != 0) {
    mask_index = dex_register_masks_.Dedup(temp_dex_register_mask_.GetRawStorage(),
                                           temp_dex_register_mask_.GetNumberOfBits());
  }
  uint32_t map_index = dex_register_maps_.Dedup(temp_dex_register_map_.data(),
                                                temp_dex_register_map_.size());
  if (!current_inline_infos_.empty()) {
    current_inline_infos_.back().dex_register_mask_index = mask_index;
    current_inline_infos_.back().dex_register_map_index = map_index;
  } else {
    current_stack_map_.dex_register_mask_index = mask_index;
    current_stack_map_.dex_register_map_index = map_index;
  }

  if (kVerifyStackMaps) {
    size_t stack_map_index = stack_maps_.size();
    int32_t depth = current_inline_infos_.size() - 1;
    // We need to make copy of the current registers for later (when the check is run).
    auto expected_dex_registers = std::make_shared<std::vector<DexRegisterLocation>>(
        current_dex_registers_.begin(), current_dex_registers_.end());
    dchecks_.emplace_back([=](const CodeInfo& code_info) {
      StackMap stack_map = code_info.GetStackMapAt(stack_map_index);
      size_t num_dex_registers = expected_dex_registers->size();
      DexRegisterMap map = (depth == -1)
        ? code_info.GetDexRegisterMapOf(stack_map, num_dex_registers)
        : code_info.GetDexRegisterMapAtDepth(depth, stack_map, num_dex_registers);
      CHECK_EQ(map.size(), num_dex_registers);
      for (size_t r = 0; r < num_dex_registers; r++) {
        CHECK_EQ(expected_dex_registers->at(r), map.Get(r));
      }
    });
  }
}

void StackMapStream::FillInMethodInfo(MemoryRegion region) {
  {
    MethodInfo info(region.begin(), method_infos_.size());
    for (size_t i = 0; i < method_infos_.size(); ++i) {
      info.SetMethodIndex(i, method_infos_[i]);
    }
  }
  if (kVerifyStackMaps) {
    // Check the data matches.
    MethodInfo info(region.begin());
    const size_t count = info.NumMethodIndices();
    DCHECK_EQ(count, method_infos_.size());
    for (size_t i = 0; i < count; ++i) {
      DCHECK_EQ(info.GetMethodIndex(i), method_infos_[i]);
    }
  }
}

size_t StackMapStream::PrepareForFillIn() {
  static_assert(sizeof(StackMapEntry) == StackMap::kCount * sizeof(uint32_t), "Layout");
  static_assert(sizeof(InvokeInfoEntry) == InvokeInfo::kCount * sizeof(uint32_t), "Layout");
  static_assert(sizeof(InlineInfoEntry) == InlineInfo::kCount * sizeof(uint32_t), "Layout");
  static_assert(sizeof(DexRegisterEntry) == DexRegisterInfo::kCount * sizeof(uint32_t), "Layout");
  DCHECK_EQ(out_.size(), 0u);

  // Read the stack masks now. The compiler might have updated them.
  for (size_t i = 0; i < lazy_stack_masks_.size(); i++) {
    BitVector* stack_mask = lazy_stack_masks_[i];
    if (stack_mask != nullptr && stack_mask->GetNumberOfBits() != 0) {
      stack_maps_[i].stack_mask_index =
        stack_masks_.Dedup(stack_mask->GetRawStorage(), stack_mask->GetNumberOfBits());
    }
  }

  size_t bit_offset = 0;
  stack_maps_.Encode(&out_, &bit_offset);
  register_masks_.Encode(&out_, &bit_offset);
  stack_masks_.Encode(&out_, &bit_offset);
  invoke_infos_.Encode(&out_, &bit_offset);
  inline_infos_.Encode(&out_, &bit_offset);
  dex_register_masks_.Encode(&out_, &bit_offset);
  dex_register_maps_.Encode(&out_, &bit_offset);
  dex_register_catalog_.Encode(&out_, &bit_offset);

  return UnsignedLeb128Size(out_.size()) +  out_.size();
}

void StackMapStream::FillInCodeInfo(MemoryRegion region) {
  DCHECK(in_stack_map_ == false) << "Mismatched Begin/End calls";
  DCHECK(in_inline_info_ == false) << "Mismatched Begin/End calls";
  DCHECK_NE(0u, out_.size()) << "PrepareForFillIn not called before FillIn";
  DCHECK_EQ(region.size(), UnsignedLeb128Size(out_.size()) +  out_.size());

  uint8_t* ptr = EncodeUnsignedLeb128(region.begin(), out_.size());
  region.CopyFromVector(ptr - region.begin(), out_);

  // Verify all written data (usually only in debug builds).
  if (kVerifyStackMaps) {
    CodeInfo code_info(region);
    CHECK_EQ(code_info.GetNumberOfStackMaps(), stack_maps_.size());
    for (const auto& dcheck : dchecks_) {
      dcheck(code_info);
    }
  }
}

size_t StackMapStream::ComputeMethodInfoSize() const {
  DCHECK_NE(0u, out_.size()) << "PrepareForFillIn not called before " << __FUNCTION__;
  return MethodInfo::ComputeSize(method_infos_.size());
}

}  // namespace art
