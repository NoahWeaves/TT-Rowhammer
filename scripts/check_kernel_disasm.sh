#!/usr/bin/env bash
# Quick disassembly check — verifies the inner hammer loop has the expected
# number of noc_async_read issue points (B4 audit).
#
# Usage:
#   ./scripts/check_kernel_disasm.sh [path-to-brisc-elf]
#
# If no path given, searches generated/ for the most recent nsided kernel ELF.
# Requires: riscv-tt-elf-objdump (or riscv32-unknown-elf-objdump)

set -euo pipefail

OBJDUMP=""
for candidate in riscv-tt-elf-objdump riscv32-unknown-elf-objdump; do
    if command -v "$candidate" &>/dev/null; then
        OBJDUMP="$candidate"
        break
    fi
done

if [[ -z "$OBJDUMP" ]]; then
    echo "ERROR: no RISC-V objdump found in PATH."
    echo "  Install riscv-tt-elf-objdump or riscv32-unknown-elf-objdump."
    exit 1
fi

ELF="${1:-}"
if [[ -z "$ELF" ]]; then
    echo "Searching for most recent nsided kernel ELF in generated/..."
    ELF=$(find "${TT_METAL_HOME:-.}/generated" -name "*.elf" -path "*brisc*" \
          -newer "${TT_METAL_HOME:-.}/rowhammer/kernels/rowhammer_nsided_kernel.cpp" \
          2>/dev/null | head -1 || true)
    if [[ -z "$ELF" ]]; then
        echo "No ELF found. Run the attack once to generate kernel binaries,"
        echo "then re-run this script (or pass the ELF path directly)."
        exit 1
    fi
    echo "  Found: $ELF"
fi

echo ""
echo "=== Disassembly: noc_async_read call sites ==="
echo ""

# Count call sites to noc_fast_read / noc_async_read in the text section
COUNT=$("$OBJDUMP" -d "$ELF" 2>/dev/null \
    | grep -cE '(noc_fast_read|noc_async_read|jal.*noc)' || echo 0)

echo "noc_async_read call sites found: $COUNT"
echo ""

if [[ "$COUNT" -eq 0 ]]; then
    echo "WARNING: zero noc_async_read calls found."
    echo "  The compiler may have inlined or optimized them away."
    echo "  Check the full disassembly:"
    echo "    $OBJDUMP -d $ELF | less"
elif [[ "$COUNT" -ge 2 ]]; then
    echo "OK: $COUNT issue points (expected 2 for double-sided, N for n-sided)."
else
    echo "WARNING: only $COUNT issue point found (expected >= 2)."
fi

echo ""
echo "=== Full inner loop context (search for 'noc' near tight loop) ==="
echo ""
"$OBJDUMP" -d "$ELF" 2>/dev/null | grep -B5 -A5 -iE '(noc_fast_read|noc_async_read)' | head -60

echo ""
echo "For full disassembly: $OBJDUMP -d $ELF | less"
