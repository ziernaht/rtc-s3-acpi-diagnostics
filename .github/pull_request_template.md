## Summary

Describe the change and why it is needed.

## Validation

- [ ] `make clean && make test` passes.
- [ ] Added or updated simulation coverage where behavior changed.
- [ ] Confirmed dry-run mode never writes `PM1_CNT.SLP_EN`.
- [ ] Distinguished simulated, emulated, and physical-hardware evidence.
- [ ] Checked the diff for credentials, personal identifiers, private paths, and machine-specific logs.

## Safety review

- [ ] The change does not make real S3 the default.
- [ ] Any real-S3 changes document the required platform resume work.
- [ ] Any loader integration change identifies the exact target project/revision and checks code-size constraints.

## Evidence / notes

List commands, results, and anything not tested.
