// SPDX-License-Identifier: MIT
#pragma once

// peekWords zero-fills an unread tail at the end of a BO. Bound the diagnostic
// request to that BO first, and report only words actually read successfully.
// This never changes the mapping or performs cache maintenance.
struct CoachzGpuHangRead {
    uint64_t handle, base;
    size_t bytes, count;
    unsigned permissions;
    IOReturn status;
};
template<class Vm>
CoachzGpuHangRead coachzGpuHangRead(Vm *vm, uint64_t address, uint32_t *out, size_t wanted) {
    CoachzGpuHangRead result{};
    result.status = kIOReturnBadArgument;
    if (!vm || !out || !wanted || (address & 3)) return result;
    result.handle = vm->handleAt(address);
    if (!result.handle) return result;
    result.status = vm->describe(result.handle, &result.base, &result.bytes, &result.permissions);
    if (result.status) return result;
    if (address < result.base || address - result.base >= result.bytes) {
        result.status = kIOReturnBadArgument;
        return result;
    }
    const size_t available = (result.bytes - size_t(address - result.base)) / 4;
    const size_t count = wanted < available ? wanted : available;
    if (!count) {result.status = kIOReturnBadArgument; return result;}
    result.status = vm->peekWords(address, out, count);
    if (!result.status) result.count = count;
    return result;
}
