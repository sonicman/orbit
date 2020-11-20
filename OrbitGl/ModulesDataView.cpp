// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ModulesDataView.h"

#include "App.h"
#include "OrbitClientData/ProcessData.h"
#include "absl/flags/flag.h"

ABSL_DECLARE_FLAG(bool, enable_frame_pointer_validator);

ModulesDataView::ModulesDataView() : DataView(DataViewType::kModules) {}

const std::vector<DataView::Column>& ModulesDataView::GetColumns() {
  static const std::vector<Column> columns = [] {
    std::vector<Column> columns;
    columns.resize(kNumColumns);
    columns[kColumnName] = {"Name", .2f, SortingOrder::kAscending};
    columns[kColumnPath] = {"Path", .5f, SortingOrder::kAscending};
    columns[kColumnAddressRange] = {"Address Range", .15f, SortingOrder::kAscending};
    columns[kColumnFileSize] = {"File Size", .0f, SortingOrder::kDescending};
    columns[kColumnLoaded] = {"Loaded", .0f, SortingOrder::kDescending};
    return columns;
  }();
  return columns;
}

std::string ModulesDataView::GetValue(int row, int col) {
  const ModuleData* module = GetModule(row);
  const MemorySpace* memory_space = module_memory_.at(module);

  switch (col) {
    case kColumnName:
      return module->name();
    case kColumnPath:
      return module->file_path();
    case kColumnAddressRange:
      return memory_space->FormattedAddressRange();
    case kColumnFileSize:
      return GetPrettySize(module->file_size());
    case kColumnLoaded:
      return module->is_loaded() ? "*" : "";
    default:
      return "";
  }
}

#define ORBIT_PROC_SORT(Member)                                                      \
  [&](int a, int b) {                                                                \
    return OrbitUtils::Compare(modules_[a]->Member, modules_[b]->Member, ascending); \
  }

#define ORBIT_MODULE_SPACE_SORT(Member)                                            \
  [&](int a, int b) {                                                              \
    return OrbitUtils::Compare(module_memory_.at(modules_[a])->Member,             \
                               module_memory_.at(modules_[b])->Member, ascending); \
  }

void ModulesDataView::DoSort() {
  bool ascending = sorting_orders_[sorting_column_] == SortingOrder::kAscending;
  std::function<bool(int a, int b)> sorter = nullptr;

  switch (sorting_column_) {
    case kColumnName:
      sorter = ORBIT_PROC_SORT(name());
      break;
    case kColumnPath:
      sorter = ORBIT_PROC_SORT(file_path());
      break;
    case kColumnAddressRange:
      sorter = ORBIT_MODULE_SPACE_SORT(start);
      break;
    case kColumnFileSize:
      sorter = ORBIT_PROC_SORT(file_size());
      break;
    case kColumnLoaded:
      sorter = ORBIT_PROC_SORT(is_loaded());
      break;
    default:
      break;
  }

  if (sorter) {
    std::stable_sort(indices_.begin(), indices_.end(), sorter);
  }
}

const std::string ModulesDataView::kMenuActionLoadSymbols = "Load Symbols";
const std::string ModulesDataView::kMenuActionVerifyFramePointers = "Verify Frame Pointers";

std::vector<std::string> ModulesDataView::GetContextMenu(int clicked_index,
                                                         const std::vector<int>& selected_indices) {
  bool enable_load = false;
  bool enable_verify = false;
  for (int index : selected_indices) {
    const ModuleData* module = GetModule(index);
    if (!module->is_loaded()) {
      enable_load = true;
    }

    if (module->is_loaded()) {
      enable_verify = true;
    }
  }

  std::vector<std::string> menu;
  if (enable_load) {
    menu.emplace_back(kMenuActionLoadSymbols);
  }
  if (enable_verify && absl::GetFlag(FLAGS_enable_frame_pointer_validator)) {
    menu.emplace_back(kMenuActionVerifyFramePointers);
  }
  Append(menu, DataView::GetContextMenu(clicked_index, selected_indices));
  return menu;
}

void ModulesDataView::OnContextMenu(const std::string& action, int menu_index,
                                    const std::vector<int>& item_indices) {
  if (action == kMenuActionLoadSymbols) {
    std::vector<ModuleData*> modules_to_load;
    for (int index : item_indices) {
      ModuleData* module_data = GetModule(index);
      if (!module_data->is_loaded()) {
        modules_to_load.push_back(module_data);
      }
    }
    GOrbitApp->LoadModules(modules_to_load);

  } else if (action == kMenuActionVerifyFramePointers) {
    std::vector<const ModuleData*> modules_to_validate;
    modules_to_validate.reserve(item_indices.size());
    for (int index : item_indices) {
      const ModuleData* module = GetModule(index);
      modules_to_validate.push_back(module);
    }

    if (!modules_to_validate.empty()) {
      GOrbitApp->OnValidateFramePointers(modules_to_validate);
    }
  } else {
    DataView::OnContextMenu(action, menu_index, item_indices);
  }
}

void ModulesDataView::DoFilter() {
  std::vector<uint32_t> indices;
  std::vector<std::string> tokens = absl::StrSplit(ToLower(filter_), ' ');

  for (size_t i = 0; i < modules_.size(); ++i) {
    const ModuleData* module = modules_[i];
    const MemorySpace* memory_space = module_memory_.at(module);
    std::string module_string = absl::StrFormat("%s %s", memory_space->FormattedAddressRange(),
                                                absl::AsciiStrToLower(module->file_path()));

    bool match = true;

    for (std::string& filter_token : tokens) {
      if (module_string.find(filter_token) == std::string::npos) {
        match = false;
        break;
      }
    }

    if (match) {
      indices.push_back(i);
    }
  }

  indices_ = indices;
}

void ModulesDataView::UpdateModules(const ProcessData* process) {
  modules_.clear();
  module_memory_.clear();
  for (const auto& [module_path, memory_space] : process->GetMemoryMap()) {
    ModuleData* module = GOrbitApp->GetMutableModuleByPath(module_path);
    modules_.push_back(module);
    module_memory_[module] = &memory_space;
  }

  indices_.resize(modules_.size());
  for (size_t i = 0; i < indices_.size(); ++i) {
    indices_[i] = i;
  }

  OnDataChanged();
}

void ModulesDataView::OnRefreshButtonClicked() {
  const ProcessData* process = GOrbitApp->GetSelectedProcess();
  if (process == nullptr) {
    LOG("Unable to refresh module list, no process selected");
    return;
  }
  GOrbitApp->UpdateProcessAndModuleList();
}

bool ModulesDataView::GetDisplayColor(int row, int /*column*/, unsigned char& red,
                                      unsigned char& green, unsigned char& blue) {
  const ModuleData* module = GetModule(row);
  if (module->is_loaded()) {
    red = 42;
    green = 218;
    blue = 130;
    return true;
  } else {
    red = 42;
    green = 130;
    blue = 218;
    return true;
  }
}
