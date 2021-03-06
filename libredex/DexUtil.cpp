/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexUtil.h"

#include <mutex>
#include <unordered_map>
#include <regex.h>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"

namespace {
static std::mutex type_system_mutex;

static std::unordered_map<const DexType*, DexClass*> type_to_class;
static std::unordered_map<const DexType*, TypeVector> class_hierarchy;
TypeVector empty_types;

}

DexType* get_object_type() {
  static DexType* object = DexType::make_type("Ljava/lang/Object;");
  return object;
}

DexType* get_void_type() {
  static DexType* v = DexType::make_type("V");
  return v;
}

DexType* get_int_type() {
  static DexType* i = DexType::make_type("I");
  return i;
}

DexType* get_long_type() {
  static DexType* j = DexType::make_type("J");
  return j;
}

DexType* get_boolean_type() {
  static DexType* z = DexType::make_type("Z");
  return z;
}

DexType* get_double_type() {
  static DexType* d = DexType::make_type("D");
  return d;
}

DexType* get_string_type() {
  static DexType* s = DexType::make_type("Ljava/lang/String;");
  return s;
}

DexType* get_class_type() {
  static DexType* c = DexType::make_type("Ljava/lang/Class;");
  return c;
}

DexType* get_enum_type() {
  static DexType* en = DexType::make_type("Ljava/lang/Enum;");
  return en;
}

bool is_primitive(DexType* type) {
  auto const name = type->get_name()->c_str();
  switch (name[0]) {
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I':
  case 'J':
  case 'F':
  case 'D':
    return true;
  case 'L':
  case '[':
  case 'V':
    return false;
  }
  not_reached();
}

DataType type_to_datatype(const DexType* t) {
  auto const name = t->get_name()->c_str();
  switch (name[0]) {
  case 'V':
    return DataType::Void;
  case 'Z':
    return DataType::Boolean;
  case 'B':
    return DataType::Byte;
  case 'S':
    return DataType::Short;
  case 'C':
    return DataType::Char;
  case 'I':
    return DataType::Int;
  case 'J':
    return DataType::Long;
  case 'F':
    return DataType::Float;
  case 'D':
    return DataType::Double;
  case 'L':
    return DataType::Object;
  case '[':
    return DataType::Array;
  }
  not_reached();
}

void build_type_system(DexClass* cls) {
  std::lock_guard<std::mutex> l(type_system_mutex);
  const DexType* type = cls->get_type();
  type_to_class.emplace(type, cls);
  const auto& super = cls->get_super_class();
  if (super) class_hierarchy[super].push_back(type);
}

DexClass* type_class(const DexType* t) {
  auto it = type_to_class.find(t);
  return it != type_to_class.end() ? it->second : nullptr;
}

char type_shorty(DexType* type) {
  auto const name = type->get_name()->c_str();
  switch (name[0]) {
  case '[':
    return 'L';
  case 'V':
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I':
  case 'J':
  case 'F':
  case 'D':
  case 'L':
    return name[0];
  }
  not_reached();
}

bool check_cast(DexType* type, DexType* base_type) {
  if (type == base_type) return true;
  const auto cls = type_class(type);
  if (cls == nullptr) return false;
  if (check_cast(cls->get_super_class(), base_type)) return true;
  auto intfs = cls->get_interfaces();
  for (auto intf : intfs->get_type_list()) {
    if (check_cast(intf, base_type)) return true;
  }
  return false;
}

bool has_hierarchy_in_scope(DexClass* cls) {
  DexType* super = nullptr;
  const DexClass* super_cls = cls;
  while (super_cls) {
    super = super_cls->get_super_class();
    super_cls = type_class_internal(super);
  }
  return super == get_object_type();
}

const TypeVector& get_children(const DexType* type) {
  const auto& it = class_hierarchy.find(type);
  return it != class_hierarchy.end() ? it->second : empty_types;
}

void get_all_children(const DexType* type, TypeVector& children) {
  const auto& direct = get_children(type);
  for (const auto& child : direct) {
    children.push_back(child);
    get_all_children(child, children);
  }
}

bool is_init(const DexMethod* method) {
  static DexString* init = DexString::make_string("<init>");
  return method->get_name() == init;
}

bool is_clinit(const DexMethod* method) {
  static DexString* clinit = DexString::make_string("<clinit>");
  return method->get_name() == clinit;
}

DexAccessFlags merge_visibility(uint32_t vis1, uint32_t vis2) {
  vis1 &= VISIBILITY_MASK;
  vis2 &= VISIBILITY_MASK;
  if ((vis1 & ACC_PUBLIC) || (vis2 & ACC_PUBLIC)) return ACC_PUBLIC;
  if (vis1 == 0 || vis2 == 0) return static_cast<DexAccessFlags>(0);
  if ((vis1 & ACC_PROTECTED) || (vis2 & ACC_PROTECTED)) return ACC_PROTECTED;
  return ACC_PRIVATE;
}

bool is_array(const DexType* type) {
  return type->get_name()->c_str()[0] == '[';
}

uint32_t get_array_level(const DexType* type) {
  auto name = type->get_name()->c_str();
  uint32_t level = 0;
  while (*name++ == '[' && ++level)
    ;
  return level;
}

DexType* get_array_type(const DexType* type) {
  if (!is_array(type)) return nullptr;
  auto name = type->get_name()->c_str();
  while (*name == '[') {
    name++;
  }
  return DexType::make_type(name);
}

bool passes_args_through(DexOpcodeMethod* insn,
                         DexCode* code,
                         int ignore /* = 0 */
                         ) {
  auto regs = code->get_registers_size();
  auto ins = code->get_ins_size();
  auto wc = insn->arg_word_count();
  if (wc != (code->get_ins_size() - ignore)) return false;
  for (int i = 0; i < wc; i++) {
    if (insn->src(i) != (regs - ins + i)) {
      return false;
    }
  }
  return true;
}

struct PenaltyPattern {
  regex_t regex;
  int penalty;
  PenaltyPattern(const char* str, int pen) {
    regcomp(&this->regex, str, 0);
    this->penalty = pen;
  }
};

std::vector<PenaltyPattern*>* compile_regexes() {
  auto rv = new std::vector<PenaltyPattern*>;
  rv->push_back(new PenaltyPattern("Layout;$", 1500));
  rv->push_back(new PenaltyPattern("View;$", 1500));
  rv->push_back(new PenaltyPattern("ViewGroup;$", 1800));
  rv->push_back(new PenaltyPattern("Activity;$", 1500));
  return rv;
}

const int kObjectVtable = 48;
const int kMethodSize = 52;
const int kInstanceFieldSize = 16;
const int kVtableSlotSize = 4;

inline bool matches_penalty(const char* string, int& penalty) {
  static std::vector<PenaltyPattern*>* patterns = compile_regexes();
  for (auto pattern : *patterns) {
    if (regexec(&pattern->regex, string, 0, nullptr, 0) == 0) {
      penalty = pattern->penalty;
      return true;
    }
  }
  return false;
}

int estimate_linear_alloc(DexClass* clazz) {
  int lasize = 0;
  /*
   * VTable guestimate.  Technically we could do better here,
   * but only so much.  Try to stay bug-compatible with
   * DalvikStatsTool.
   */
  if (!(clazz->get_access() & DEX_ACCESS_INTERFACE)) {
    int vtablePenalty = kObjectVtable;
    if (!matches_penalty(clazz->get_type()->get_name()->c_str(), vtablePenalty)
        && clazz->get_super_class() != nullptr) {
      /* what?, we could be redexing object some day... :) */
      matches_penalty(
          clazz->get_super_class()->get_name()->c_str(), vtablePenalty);
    }
    lasize += vtablePenalty;
    lasize += clazz->get_vmethods().size() * kVtableSlotSize;
  }
  /* Dmethods... */
  lasize += clazz->get_dmethods().size() * kMethodSize;
  /* Vmethods... */
  lasize += clazz->get_vmethods().size() * kMethodSize;
  /* Instance Fields */
  lasize += clazz->get_ifields().size() * kInstanceFieldSize;
  return lasize;
}


Scope build_class_scope(const DexClassesVector& dexen) {
  Scope v;
  for (auto const& classes : dexen) {
    for (auto clazz : classes) {
      v.push_back(clazz);
    }
  }
  return v;
}

void post_dexen_changes(const Scope& v, DexClassesVector& dexen) {
  std::unordered_set<DexClass*> clookup(v.begin(), v.end());
  for (auto& classes : dexen) {
    classes.erase(
      std::remove_if(
        classes.begin(),
        classes.end(),
        [&](DexClass* cls) {
          return !clookup.count(cls);
        }),
      classes.end());
  }
  if (debug) {
    std::unordered_set<DexClass*> dlookup;
    for (auto const& classes : dexen) {
      for (auto const& cls : classes) {
        dlookup.insert(cls);
      }
    }
    for (auto const& cls : clookup) {
      assert_log(dlookup.count(cls), "Can't add classes in post_dexen_changes");
    }
  }
}
