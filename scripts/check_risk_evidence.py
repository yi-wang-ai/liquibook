"""Validate Phase 2A test and paired-benchmark evidence before tagging."""
import json
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
out = root / 'results/risk-controls'
variants = ('Debug', 'Release', 'ASan', 'UBSan', 'ClangDebug')
tests = {}
for variant in variants:
    text = (out / (variant+'-tests.txt')).read_text()
    assert '100% tests passed, 0 tests failed out of 10' in text
    assert '157 test cases out of 157 passed' in text
    assert '2671 assertions out of 2671 passed' in text
    risk = text.split('Test module "PreTradeRiskTests"')[1]
    assert '30 test cases out of 30 passed' in risk
    assert '325 assertions out of 325 passed' in risk
    cases = dict(re.findall(r'Test case "([^"]+)" has (passed|failed)', risk))
    assert len(cases) == 30 and set(cases.values()) == {'passed'}
    assert not re.search(r'ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer|Caught exception', text)
    tests[variant] = cases
for variant in variants:
    assert tests[variant] == tests['Debug']
rows = json.loads((out / 'benchmark_raw.json').read_text())
assert len(rows) == 28
for workload in ('crossing', 'resting'):
    for repeat in range(1,8):
        pair = [r for r in rows if r['workload'] == workload and r['repeat'] == repeat]
        assert len(pair) == 2 and {r['mode'] for r in pair} == {'bare','risk'}
        assert pair[0]['workload_hash'] == pair[1]['workload_hash']
        assert pair[0]['result_hash'] == pair[1]['result_hash']
        for row in pair:
            assert row['orders'] == 1000000 and row['seed'] == 20261001
            assert row['throughput_orders_s'] > 0 and row['peak_rss_kib'] > 0
            assert row['p50_ns'] <= row['p95_ns'] <= row['p99_ns'] <= row['p99_9_ns']
subprocess.run(['git', 'diff', '--exit-code', 'reproduction-verified', '--', 'src'], cwd=root, check=True)
(out / 'verification.json').write_text(json.dumps({
    'risk_tests_by_configuration': tests,
    'risk_assertions_per_run': 325,
    'upstream_assertions_per_run': 2671,
    'upstream_disabled_placeholder': 'TestTwoAonBidTwoAonAsk',
    'paired_benchmark_trials': 14,
    'orders_per_trial': 1000000,
    'all_paired_outcomes_match': True,
    'upstream_src_unchanged': True,
}, indent=2) + '\n')
print('Verified five test configurations, 14 matched benchmark pairs, and unchanged upstream src.')
