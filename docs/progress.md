# Progress record

Short log of how the release was produced, so work can resume after a break.
Evidence (commands, outputs, counts) is in `docs/verification.md`.

| Milestone | State | Notes |
|---|---|---|
| 0 Discovery | done | WSL2 Ubuntu 26.04; assignment PDF (3 pages) read, matches the brief. Toolchain initially missing (no compiler, universe index never fetched); installed by the user with apt. |
| 1 Specification | done | `docs/requirements.md` — scope, schemas, caps, exit codes, FR/NFR ids. |
| 2 Core behaviour | done | Written before the toolchain existed, then verified in one pass: 93 unit tests green on first build after fixing test-only warnings. |
| 3 Component hardening | done | 85 → 96 pytest tests; mutation check 9/10 → 10/10; ASan contaminates `RLIMIT_CORE` (configured with `disable_coredump=0`). |
| 4 Completion | done | Docs, essay, verification record, independent AI review (F1–F9 fixed or documented), clean-clone README check. |
| 5 Second review | done (author steps open) | External static review: verify.sh could pass without tests, launcher/limit and pipe-capacity assumptions, overstated guarantees, essay authorship. All fixed or narrowed; suite now run under nofile limits down to 128:128. Author must write the essay's marked sections and do the DEVELOPER_NOTES exercises. |

## Decisions taken during the work

* Coarse first commits rather than fabricated intermediate history.
* `-Wnull-dereference` kept for production code, dropped for tests
  (false positives in inlined `std::optional` copies at `-O2`).
* Exact-descriptor component tests (`exact_fds`) run only against the normal
  build.
* One injectable seam added (`readdir`) after the mutation check showed an
  untested branch.

## Open items (not blocking 0.1.0)

* Essay: author's contribution statement and reflection (marked in
  `docs/essay.md`).
* DEVELOPER_NOTES exercise E3 (demo fixture over-skips at low limits).
* No licence chosen for the project.
* Nothing pushed or published; no CI.
* Roadmap hardening items (atomic report write, `O_PATH` contract open,
  fuzzing, multi-distro CI).
