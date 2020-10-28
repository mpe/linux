// SPDX-License-Identifier: GPL-2.0-only

#include "processor.h"
#include "kvm_util.h"
#include "../kvm_util_internal.h"


void virt_pgd_alloc(struct kvm_vm *vm, uint32_t memslot)
{
	// FIXME
}

void virt_pg_map(struct kvm_vm *vm, uint64_t gva, uint64_t gpa,
		 uint32_t memslot)
{
	// FIXME
}

vm_paddr_t addr_gva2gpa(struct kvm_vm *vm, vm_vaddr_t gva)
{
	// FIXME
	return 0;
}

void virt_dump(FILE *stream, struct kvm_vm *vm, uint8_t indent)
{
	// FIXME
}

void vcpu_dump(FILE *stream, struct kvm_vm *vm, uint32_t vcpuid, uint8_t indent)
{
	struct vcpu *vcpu = vcpu_find(vm, vcpuid);

	if (!vcpu)
		return;
}
