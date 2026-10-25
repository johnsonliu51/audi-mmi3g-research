#!/usr/bin/env python3
"""Read pinned Toolkit showScreen bytes and inventory observed LM requests.

This is an offline evidence extractor. It neither executes the ELF nor opens
QNX device nodes. Operation names are hypotheses from messages/call context,
not a recovered public interface. It emits no C header or callable wrapper.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

REFERENCE_SHA256 = "033174eb41f30a6197aac7fafcfbe22adcdeb0c637e9a43937ee90d20fd9bdd5"
REFERENCE_SIZE = 53120
REFERENCE_COMMIT = "e9ca3469835e3e1c78dabfb5cbe5676d2b752500"
UPSTREAM = ("https://github.com/dspl1236/MMI3G-Toolkit/blob/" +
            REFERENCE_COMMIT + "/core/bin/showScreen")
GENERIC_REQUEST = 0x080428CC

# These locations were identified by PT_LOAD-based SH4 disassembly. The tool
# verifies the relevant instructions and literal words in the pinned sample.
# role, wrapper start, R0 target load, R5 code load, R7 size move, expected code
REQUESTS = (
    ("hmi_version_exchange_candidate", 0x08041F4C, 0x08041FCC,
     0x08041FD0, 0x08041FD6, 0xC00C0506),
    ("register_displayable_candidate", 0x080420E8, 0x08042126,
     0x08042128, 0x0804212C, 0xC0140500),
    ("displayable_list_operation_unknown", 0x08042160, 0x080421E2,
     0x080421E4, 0x080421E8, 0x800C0505),
    ("unregister_displayable_candidate", 0x0804222C, 0x08042250,
     0x08042252, 0x08042256, 0x80040501),
    ("three_word_operation_unknown", 0x0804227C, 0x080422B0,
     0x080422B2, 0x080422B6, 0x800C0502),
    ("get_vfb_candidate", 0x080422E0, 0x0804231C,
     0x0804231E, 0x08042322, 0xC0100503),
    ("update_vfb_candidate", 0x08042358, 0x0804237C,
     0x0804237E, 0x08042382, 0x80040504),
    ("one_word_operation_7_unknown", 0x080423A8, 0x080423CC,
     0x080423CE, 0x080423D2, 0x80040507),
    ("one_word_operation_8_unknown", 0x080423F8, 0x0804241C,
     0x0804241E, 0x08042422, 0x80040508),
)


class ElfReader:
    def __init__(self, data):
        if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
            raise ValueError("requires ELF32 little-endian")
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
        if header[2] != 42 or header[9] != 32 or header[10] > 256:
            raise ValueError("requires EM_SH and ordinary ELF32 program headers")
        if header[5] + header[9] * header[10] > len(data):
            raise ValueError("program header table lies outside file")
        self.data = data
        self.loads = []
        for index in range(header[10]):
            segment = struct.unpack_from("<IIIIIIII", data,
                                         header[5] + index * header[9])
            if segment[0] != 1:
                continue
            if segment[1] + segment[4] > len(data) or segment[4] > segment[5]:
                raise ValueError("invalid file-backed PT_LOAD segment")
            self.loads.append(segment)

    def read(self, address, count):
        matches = [s for s in self.loads
                   if s[2] <= address and address + count <= s[2] + s[4]]
        if len(matches) != 1:
            raise ValueError("address range has no unique file-backed PT_LOAD: " +
                             hex(address))
        offset = matches[0][1] + address - matches[0][2]
        return self.data[offset:offset + count]

    def word(self, address):
        return struct.unpack("<H", self.read(address, 2))[0]

    def longword(self, address):
        return struct.unpack("<I", self.read(address, 4))[0]

    def pc_literal(self, address, register):
        instruction = self.word(address)
        if (instruction & 0xFF00) != (0xD000 | (register << 8)):
            raise ValueError("unexpected SH mov.l PC instruction at " + hex(address))
        pool = ((address + 4) & ~3) + (instruction & 0xFF) * 4
        return pool, self.longword(pool)


def request_record(reader, spec):
    role, wrapper, target_pc, code_pc, size_pc, expected_code = spec
    target_pool, target = reader.pc_literal(target_pc, 0)
    code_pool, code = reader.pc_literal(code_pc, 5)
    if target != GENERIC_REQUEST or code != expected_code:
        raise ValueError("request target or code disagrees with pinned evidence")
    if reader.word(size_pc - 2) != 0x400B:  # jsr @r0; size is delay-slot move
        raise ValueError("unexpected request call at " + hex(size_pc - 2))
    size_instruction = reader.word(size_pc)
    if (size_instruction & 0xFF00) == 0xE700:  # mov #imm,r7
        size = size_instruction & 0xFF
        if size > 127:
            raise ValueError("unexpected negative immediate payload size")
        length = {"kind": "immediate_bytes", "value": size}
    elif size_instruction == 0x67A3:  # mov r10,r7
        length = {
            "kind": "register_r10",
            "manual_disassembly_observation":
                "R10 = 4 * input_count + 8; input_count == 0 overrides R10 to 12",
            "unresolved": "purpose of list operation and zero-count third word",
        }
    else:
        raise ValueError("unexpected R7 payload-length instruction")
    return {
        "candidate_role": role,
        "role_is_public_contract": False,
        "wrapper_va": hex(wrapper),
        "request_code_load_va": hex(code_pc),
        "request_code_pool_va": hex(code_pool),
        "request_code": hex(code),
        "call_va": hex(size_pc - 2),
        "request_target_pool_va": hex(target_pool),
        "request_target_va": hex(target),
        "payload_length": length,
    }


def report(path):
    if path.stat().st_size != REFERENCE_SIZE:
        raise ValueError("size mismatch; addresses apply only to pinned showScreen")
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != REFERENCE_SHA256:
        raise ValueError("SHA-256 mismatch; do not apply this inventory to another binary")
    reader = ElfReader(data)
    if reader.read(0x0804C380, 18) != b"/dev/layermanager\x00":
        raise ValueError("layer-manager path mismatch")
    # Cleanup constructs [40, 16, 19] and passes 40 separately to the candidate
    # unregister operation. Preserve raw instruction facts without naming IDs.
    for address, opcode in ((0x08041D12, 0xE128), (0x08041D16, 0xE110),
                            (0x08041D1A, 0xE113), (0x08041D3C, 0xE428)):
        if reader.word(address) != opcode:
            raise ValueError("cleanup constant instruction mismatch")
    return {
        "schema": "mmi3g.toolkit.display_static_evidence.v1",
        "mode": "offline_pinned_sample_only",
        "source": str(path),
        "source_sha256": digest,
        "source_bytes": len(data),
        "upstream": UPSTREAM,
        "runtime_compatibility": "unverified_on_HN+_CN_AU3G_P0710",
        "layer_manager_path_observed": "/dev/layermanager",
        "requests": [request_record(reader, spec) for spec in REQUESTS],
        "cleanup_observation": {
            "function_va": "0x8041d04",
            "candidate_unregister_argument": 40,
            "list_arguments": [40, 16, 19],
            "list_count": 3,
            "list_other_argument": 0,
            "before_draw_and_after_wait": True,
            "ownership_and_restore_semantics": "unknown; not safe to reuse directly",
        },
        "generic_request_observation": {
            "function_va": hex(GENERIC_REQUEST),
            "clock_call_numeric_id": 2,
            "timeout_call_numeric_flags": 80,
            "elapsed_limit_ns_literal": struct.unpack("<Q", reader.read(0x0804C3A0, 8))[0],
            "relative_timeout_ns_literal": struct.unpack("<Q", reader.read(0x0804C3A8, 8))[0],
            "retry_numeric_return": 4,
            "note": "numeric constants are observations, not SDK enum definitions",
        },
        "not_recovered": [
            "legal matching LM header and named payload fields",
            "displayable ID allocation/ownership rules",
            "meaning of register argument 2 and response words",
            "focus/list state and exact original-HMI restore operation",
            "server cleanup after client crash or timeout",
            "P0710 compatibility and graphics ABI version",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path, help="the pinned upstream showScreen ELF")
    args = parser.parse_args()
    try:
        result = report(args.file)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, "error: " + str(error) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
