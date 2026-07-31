#pragma once
#include <stdint.h>
#include <stddef.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
void ioapic(struct acpi_table_madt* madt);