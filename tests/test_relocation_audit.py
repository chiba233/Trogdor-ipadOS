import os
from pathlib import Path
import tempfile
import unittest
from test_xnu_checkpoint import ROOT
from build import CPU
from relocation_audit import audit_sources,inspect_object


class RelocationAuditTests(unittest.TestCase):
    def compile(self,source):
        zig=ROOT/'.tools/ziglang/ziglang/zig'
        env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'.tools/zig-cache'),ZIG_LOCAL_CACHE_DIR=str(ROOT/'.tools/zig-local-cache'))
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'fixture.c';path.write_text(source)
            return audit_sources(zig,CPU,[path],env,Path(tmp)/'objects')

    def test_pointer_table_in_movable_image_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'absolute symbol relocation 257'):
            self.compile('const char *table[]={"first","second"};const char *get(unsigned i){return table[i&1];}')

    def test_inline_strings_pc_relative_references_and_fixed_kernel_values_are_safe(self):
        report=self.compile('static const char table[][12]={"first","second"};'
            'const char *get(unsigned i){return table[i&1];}'
            'const char *literal(void){return "literal";}'
            'unsigned long value(unsigned i){static const unsigned long fixed[]={0xfffffff007948410ul,0x17000000ul};return fixed[i&1];}')
        self.assertEqual(report['fixture.c']['absolute_symbol_relocations'],0)
        self.assertGreater(report['fixture.c']['allocated_relocations_checked'],0)

    def test_non_object_is_not_accepted_as_an_audit(self):
        for data in (b'',b'\x7fELF\x02\x01',bytes(64),b'\x7fELF\x02\x01'+bytes(58)):
            with self.assertRaises(ValueError):inspect_object(data)
