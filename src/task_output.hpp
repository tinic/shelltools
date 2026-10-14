#pragma once

#include "task_snapshot.hpp"

namespace shelltools {

void print_task_header();
void print_task(const TaskRow &row, bool machine, LONG snapshot_number);

} // namespace shelltools
