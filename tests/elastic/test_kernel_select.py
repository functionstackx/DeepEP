"""CPU-only tests: python tests/elastic/test_kernel_select.py"""

import ast
import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[2]


class KernelSelectTests(unittest.TestCase):
    def test_cpp_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'test_kernel_select')
            subprocess.run([
                os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                f'-I{ROOT / "deep_ep/include"}',
                str(ROOT / 'tests/elastic/test_kernel_select.cpp'),
                str(ROOT / 'tests/elastic/kernel_select_peer.cpp'),
                '-o', binary,
            ], check=True)
            subprocess.run([binary], check=True)

    def test_python_qp_allocation(self):
        # Execute the real constructor's QP block without importing torch or _C.
        tree = ast.parse((ROOT / 'deep_ep/buffers/elastic.py').read_text())
        cls = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'ElasticBuffer')
        init = next(node for node in cls.body if isinstance(node, ast.FunctionDef) and node.name == '__init__')
        start = next(i for i, node in enumerate(init.body)
                     if isinstance(node, ast.Assign)
                     and any(isinstance(target, ast.Name) and target.id == 'ordered_hybrid'
                             for target in node.targets))
        block = compile(ast.Module(body=init.body[start:start + 2], type_ignores=[]), '<QP allocation>', 'exec')
        for hybrid in (False, True):
            for ordered in (False, True):
                for fast_atomic in (False, True):
                    for requested in (0, 1, 2, 11, 17, 129):
                        with self.subTest(hybrid=hybrid, ordered=ordered, fast_atomic=fast_atomic, requested=requested):
                            calls = []

                            def select():
                                calls.append(True)
                                return ordered

                            scope = {
                                'self': SimpleNamespace(allow_hybrid_mode=hybrid),
                                '_C': SimpleNamespace(use_ordered_hybrid_kernel=select,
                                                      min_unordered_gin_qps=2, max_unordered_gin_qps=17),
                                'check_fast_rdma_atomic_support': lambda: fast_atomic,
                                'num_allocated_qps': requested,
                            }
                            with contextlib.redirect_stdout(io.StringIO()):
                                exec(block, scope)
                            if not hybrid:
                                expected = requested or 17
                            elif ordered:
                                expected = requested or (65 if fast_atomic else 129)
                            else:
                                expected = max(2, min(requested, 17)) if requested else 0
                            self.assertEqual(scope['num_allocated_qps'], expected)
                            self.assertEqual(len(calls), int(hybrid))


if __name__ == '__main__':
    unittest.main()
