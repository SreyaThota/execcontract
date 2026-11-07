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
| 5 Second review | engineering fixes complete | External static review: verify.sh could pass without tests, launcher/limit and pipe-capacity assumptions, overstated guarantees, essay authorship. All fixed or narrowed; suite now run under nofile limits down to 128:128. The assisted essay is complete; E1/E2 were reproduced by the coding agent, without a claim of author execution. |
| 6 Final audit | done | Added the borrowed-descriptor non-interference test and a fixed-seed mutation test of the contract path; CI workflow (Ubuntu 24.04, GCC 13); 17-mutant check; consolidated verification record; README architecture/decision sections; `docs/INTERVIEW_PREPARATION.md`. |

## Decisions taken during the work

* Coarse first commits rather than fabricated intermediate history.
* `-Wnull-dereference` kept for production code, dropped for tests
  (false positives in inlined `std::optional` copies at `-O2`).
* Exact-descriptor component tests (`exact_fds`) run only against the normal
  build.
* One injectable seam added (`readdir`) after the mutation check showed an
  untested branch.

## Submission status and remaining work

* Engineering, tests, verification and documentation are complete for 0.1.0
  (final code commit `9ccb680`; evidence in `docs/verification.md`).
* All recorded runs, including E1/E2, were executed by AI agents; the
  author's own rehearsal (`DEVELOPER_NOTES.md` section 7) is not recorded.
* MIT licence; public repository https://github.com/SreyaThota/execcontract.
* CI is defined; its first run happens when the final commits are pushed.
* Remaining hardening is in `ROADMAP.md` (atomic report write, `O_PATH`
  contract open, coverage-guided fuzzing, broader CI).
