# CI and repository rules

This page lists every workflow, every required check and the rules of the repository. A change to a workflow or a check updates this page in the same pull request.

## Workflows

| File | Runs on | Jobs |
| :--- | :--- | :--- |
| `commits.yaml` | pull requests | Conventional commits |
| `prformat.yaml` | pull requests | PR format |
| `config.yaml` | pull requests, `main`, manual | Actionlint, Zizmor, ShellCheck, File names, Renovate config |
| `security.yaml` | pull requests, `main`, weekly, manual | Secret scan (gitleaks), OSSF Scorecard |

- Every workflow starts with `permissions: {}` and grants each job only what it needs. Every action is pinned to a full commit SHA with its version in a comment, and checkouts do not keep the token.
- `commits.yaml` and `prformat.yaml` never cancel a run, because GitHub keeps a cancelled run of a required check as unmet.
- OSSF Scorecard runs only on `main` and never on a pull request. It is not a required check.
- The tool images that workflows start with `docker run` are listed in `.github/tools/Dockerfile`, so Renovate can update them. The script `.github/tools/image.sh` prints the image of one stage.
- A version in a workflow that has a `renovate` comment above it is updated by Renovate through the custom manager in `renovate.json`.
- The workflows for the device builds, the tests, the dependency review and the releases are added with the first device folder. Dependency review needs the dependency graph of the repository and a manifest to read, and neither exists yet.
- The Dockerfile in `.github/tools` is never built, so it has no Hadolint job.
- The Actionlint version is pinned in `config.yaml` with a `renovate` comment, so Renovate raises it like the other tools.

## Required checks

The ruleset `main` requires a pull request, resolved review threads, verified signatures and an up to date branch. It allows only merge commits. Nobody can bypass it. These status checks are required, each pinned to GitHub Actions.

| Check | Workflow |
| :--- | :--- |
| Conventional commits | `commits.yaml` |
| PR format | `prformat.yaml` |
| Actionlint | `config.yaml` |
| Zizmor | `config.yaml` |
| ShellCheck | `config.yaml` |
| File names | `config.yaml` |
| Renovate config | `config.yaml` |
| Secret scan (gitleaks) | `security.yaml` |

A check is added to the ruleset after it has run once on a pull request, because a required check that never ran blocks every merge. OSSF Scorecard is not required because it never runs on a pull request.

## Tags and releases

The rulesets `release-tags` and `release-tags-creation` protect the tags that match `v*` and `*-v*`. Nobody can delete, move or update such a tag, and only the release GitHub App can create one. Immutable releases are on.

## Commit and pull request rules

The rules are in [CONTRIBUTING.md](../CONTRIBUTING.md). Two checks enforce them. Conventional commits lints every commit message and the title of the pull request. PR format checks the branch name, the label, the assignee and the body.

## File names

The File names job runs ls-lint with the rules in `.ls-lint.yml`.

- Folders are lowercase words joined by hyphens, and a leading dot marks the folder of a tool.
- Files are lowercase words joined by hyphens, and a dot may mark a variant.
- A Markdown file may also be written in capitals joined by underscores, such as `README.md`.
- YAML files end with `.yml`, except the workflows, which end with `.yaml`. The name of the workflow file is part of the identity that signs a release file.
- The issue forms in `.github/ISSUE_TEMPLATE` are not checked. Files with no extension, such as `LICENSE`, `CODEOWNERS` and `Dockerfile`, are not checked either.

## Dependency updates

Renovate opens the pull requests for dependency updates on Monday before 07:00 Asia/Jakarta, waits 7 days after a release, and groups minor and patch updates of actions. Two cases skip the wait. Images from `ghcr.io` have no release time, so they are not held, and a pull request that fixes a known vulnerability does not wait for the schedule or the age. Renovate rebases a branch only when it conflicts, so a branch that falls behind after a merge is updated with the update branch button, one pull request at a time, because every update starts a full run of the checks. Dependabot alerts are on, and Dependabot does not open pull requests, so Renovate opens the fix for a vulnerable dependency.
