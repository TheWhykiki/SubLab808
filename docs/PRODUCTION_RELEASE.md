# Production release contract v2

Code implementation does not authorize publication. Keep the current PRs as
drafts until independent review is complete. Do not merge, create a tag, dispatch
the production workflow, or publish a prerelease without separate owner approval.

## Separate owner permission for Stable/Latest

The physical environment review is independent QA, not the owner's publication
permission. After the physical checks have actually finished, while the
`physical-daw-release` job is still awaiting its review, the repository owner
posts a commit comment on the exact candidate commit. The comment is the prefix
`whykiki-stable-promotion-v2:` immediately followed by this JSON (use real values):

```json
{"schemaVersion":2,"repository":"TheWhykiki/PRODUCT","runId":123,"runAttempt":1,"releaseId":456,"tag":"vVERSION","commit":"40-lowercase-hex","assetManifestSha256":"64-lowercase-hex","decision":"approve"}
```

All values are shown in the staging job's summary. This is a comment on the
commit, not a PR comment. An example command after deliberately replacing the
placeholders is `gh api --method POST repos/OWNER/PRODUCT/commits/COMMIT/comments
-f body='whykiki-stable-promotion-v2:{...}'`. Never run it automatically.

The validator checks GitHub's immutable owner user ID, login, commit ID and every
binding field. An unrelated or older run cannot approve the candidate. The
latest matching owner comment is authoritative; `"decision":"reject"` revokes
an earlier approval. Duplicate JSON keys fail closed.

Only then does the independent reviewer submit the physical receipt and approve
the physical environment. The finalizer requires both approvals and all automated
gates. Missing owner permission leaves the candidate quarantined as an immutable
prerelease. Do not rerun blindly: a new run attempt needs new bindings and receipts.
Do not delete or reuse the tag of any published candidate.

## Protected GitHub setup

On 2026-09-26, both repositories' `main` branch protection was configured and
verified through GitHub: native platform CI plus release parity are mandatory,
checks are bound to GitHub Actions, branches must be current, at least one PR
review is required, stale approvals are dismissed, last-push approval and
conversation resolution are required, administrators cannot bypass, and force
pushes/deletions are disabled.
Repository release immutability was also enabled and re-read as enabled for both
repositories. This affects future releases; no release or tag was created.

Before signing, both `release-signing` and `physical-daw-release` must have
explicit independent user reviewers, self-review prevention, no admin bypass,
and protected branches only. They are deliberately not activated with a dummy
reviewer. The authorization, staging and promotion steps re-read protection and
fail closed if it has weakened. The Administration-read token for this preflight
must remain separate from release-signing credentials.

## External acceptance still required

- Name an independent reviewer with repository read access.
- Provision the Azure tenant/subscription, validated Public Trust certificate
  profile, environment-bound GitHub OIDC roles and versioned non-exportable
  P-256 EC-HSM release-gate key. Do not upload Windows PFX or gate private keys.
- Supply the actual Apple Developer IDs, complete current/next signer pairs and
  notarization credentials through protected secrets, not source files.
- Establish any required WiX Open Source Maintenance Fee sponsorship before
  distribution, according to the organization's revenue.
- Run the immutable candidates on all four physical systems in both hosts,
  preserve the twelve receipts and the requested 64-/8-preset listening evidence.
- Bootstrap only at SubLab808 v1.4.0 / ReverseLab v1.1.0, then separately authorize
  v1.4.1 / v1.1.1 to prove the real N→N+1 path. ReverseLab v1.0.4 is not a trusted
  updater baseline; reinstall manually.

Unsigned CI and local host harnesses are development evidence. They do not prove
Azure signing, Apple notarization, physical Cubase/REAPER behavior, or that the
products are release-approved.
