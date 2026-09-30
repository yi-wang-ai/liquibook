"""Validate every upstream test across the saved before/after build matrix."""
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
expected_failures = {
    'TestTradeCallbacks', 'TestStopOrdersOffMarketNoTrade',
    'TestStopMarketOrdersTradeWhenStopPriceReached', 'TestStopOrdersCancelBeforeTrigger'
}
variants = ['cmake-Debug', 'cmake-Release', 'cmake-ASan', 'cmake-UBSan',
            'cmake-ClangDebug', 'mpc-Debug', 'mpc-Release', 'mpc-ASan', 'mpc-UBSan']
summary = {}
all_names = None
for phase in ('baseline', 'repaired'):
    summary[phase] = {}
    for variant in variants:
        folder = root / 'diagnostics' / phase / variant
        text = (folder / 'full-suite.txt').read_text()
        statuses = dict(re.findall(r'Test case "([^"]+)" has (passed|failed)', text))
        assert len(statuses) == 157, (phase, variant, len(statuses))
        if all_names is None:
            all_names = set(statuses)
        assert set(statuses) == all_names
        failed = {name for name, status in statuses.items() if status == 'failed'}
        assert failed == (expected_failures if phase == 'baseline' else set()), (phase, variant, failed)
        assert int((folder / 'full-suite.exit').read_text()) == (201 if phase == 'baseline' else 0)
        for name in expected_failures:
            assert int((folder / (name + '.exit')).read_text()) == (201 if phase == 'baseline' else 0)
        assert not re.search(r'ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer', text)
        summary[phase][variant] = statuses
    repro = root / 'diagnostics' / phase / 'reproducers'
    exits = list(repro.glob('*.exit'))
    assert len(exits) == 56, (phase, len(exits))
    for file in exits:
        expected = 0 if phase == 'repaired' or 'trade_price' in file.name else 1
        assert int(file.read_text()) == expected, (file, expected)
        text = file.with_suffix('.txt').read_text()
        assert not re.search(r'ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer', text)
summary['coverage_notes'] = {
    'registered_cases': 157,
    'cases_with_assertions': 156,
    'upstream_disabled_body': 'TestTwoAonBidTwoAonAsk',
    'assertions_after_repair': 2671,
}
(root / 'diagnostics' / 'test-accounting.json').write_text(json.dumps(summary, indent=2) + '\n')
print('All 157 tests accounted for in 18 full-suite runs; 112 independent reproducer runs verified.')
