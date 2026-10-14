#pragma once

#include "task_snapshot.hpp"

namespace shelltools {

void print_task_header(bool show_cpu = false);
void print_task(const TaskRow &row, bool machine, LONG snapshot_number, LONG cpu_tenths = -1,
                bool detail = false);

} // namespace shelltools
