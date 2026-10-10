"""An SSE compare's flags must survive a join of different comiss operands.

FP's pickup proximity test (0x000C15B0) reaches one `jbe` from two `comiss`
sites that compare different registers. The join was left unmerged, the jbe
read the never-assigned `_flags` and was never taken, and the radius check
it guards was skipped: every pickup in the level was collected at load.
"""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand

BASE = 0x10000


def translate_join(second=bytes.fromhex('0f2fd3')):
    # test ecx,ecx; jz alt; comiss xmm0,xmm1; jmp join;
    # alt: comiss xmm2,xmm3 (or `second`); join: jbe +1; ret; ret
    image = (bytes.fromhex('85c974050f2fc1eb03') + second
             + bytes.fromhex('7601c3c3'))
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='flag-join-sse-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_different_comiss_operands_join_for_jbe():
    code = translate_join()
    assert '(_fca <= _fcb)' in code, code
    assert '_flags /* jbe' not in code, code


def test_comiss_and_ucomiss_join():
    code = translate_join(bytes.fromhex('0f2ed3'))
    assert '(_fca <= _fcb)' in code, code


def test_comiss_and_cmp_are_not_merged():
    x0 = Operand(type='reg', reg='xmm0')
    x1 = Operand(type='reg', reg='xmm1')
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('comiss', [x0, x1]), ('cmp', [a, b])]) is None
