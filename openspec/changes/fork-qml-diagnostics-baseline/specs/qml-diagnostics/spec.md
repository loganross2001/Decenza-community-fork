## ADDED Requirements

### Requirement: The Baseline Reflects The Tree It Gates

`qml-diagnostics-baseline.json` SHALL describe the QML tree it is checked against.
Its `clean` list and per-file ceilings SHALL be those the gate tool reports on that
exact tree, from a build in which types fully resolve.

The baseline is a generated snapshot, not a hand-authored list, so it cannot be
correct for a tree other than the one it was generated on. A downstream tree that
adds QML the upstream tree lacks — additional components, or edits to shared files —
carries diagnostics no upstream-generated baseline can account for, so importing
that baseline makes the gate unpassable on the downstream tree while asserting it is
green on the upstream one.

When an upstream integration replaces the baseline, the file SHALL be regenerated on
the integrating tree with `--update-baseline`, from a run the tool certifies as
fully resolving, rather than accepted as merged. This is the same discipline the
project applies to its other generated or append-only registries, whose textually
clean merges are not semantically clean.

Regeneration SHALL NOT be used to launder a genuine `unqualified` regression into an
accepted count: the "`unqualified` Is Enforced Per File" and "The `unqualified`
Category Is Never Exempt" requirements still hold, so identifiers are made resolvable
first and only the delegate-scope residue is recorded.

#### Scenario: An integration imports a baseline generated on another tree

- **WHEN** a sync brings in a `qml-diagnostics-baseline.json` regenerated on the
  upstream tree, and the integrating tree has QML files or edits the upstream tree
  does not
- **THEN** the baseline is regenerated on the integrating tree from a fully-resolving
  build, and the gate passes on that tree — the imported file is not accepted as-is

#### Scenario: A downstream-only file is absent from the imported baseline

- **WHEN** the gate reports a registered QML file as "new" because the imported
  baseline predates it on this tree
- **THEN** the file's genuine `unqualified` accesses are resolved to zero and the
  baseline is regenerated to record it, rather than the file being added to the
  baseline at a non-zero count

#### Scenario: Regeneration is attempted from an under-resolving build

- **WHEN** `--update-baseline` is run on a build where types do not fully resolve
- **THEN** the tool's staleness guard refuses the update, and it is not overridden —
  because an under-resolving run misreports the very counts the baseline would record
