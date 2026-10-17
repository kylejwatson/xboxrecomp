# Third-party licence texts

`LGPL-2.1.txt` is the verbatim GNU Lesser General Public License v2.1, from

    https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

It applies to the files listed under "LGPL-2.1-or-later" in the top-level
NOTICE — the MCPX APU sources and `src/nv2a/nv2a_regs.h`, extracted from xemu
and remaining the copyright of espes, Jannik Vogel and Matt Borgerson.

Shipping the verbatim text alongside those files is an LGPL requirement, not
a courtesy.

`Apache-2.0.txt` (the SPDX text of the Apache License 2.0) and
`glslang-LICENSE.txt` (glslang's own LICENSE.txt, as its distribution packages
install it) are for the shareable Linux build, which carries shaderc,
SPIRV-Tools (both Apache-2.0) and glslang beside the runtime because few
distributions install them; the title's make_kit.sh copies them into the
build's LICENSES/ with an index (THIRD_PARTY.txt).
