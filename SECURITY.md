# Security Policy

## Supported versions

Only the latest release of each device and the `main` branch receive security fixes.

## Reporting a vulnerability

Please do not open a public issue for a security problem.

Use GitHub private vulnerability reporting instead. Open the [Security tab](https://github.com/aethera-deeptech/firmware/security) and choose **Report a vulnerability**, or go straight to the [advisory form](https://github.com/aethera-deeptech/firmware/security/advisories/new). The report stays private. Only you, the people who manage security advisories for this repository, and anyone who is added to the report can see it.

A good report includes the following.

- A description of the problem and its impact, for example a way to switch a relay without permission, a leaked key or a way to read the traffic between the devices.
- Steps to reproduce it, or a proof of concept.
- The device, the firmware version or the commit you tested.
- Relevant logs with any secrets removed.

The report is triaged, fixed in private when it is valid, and, where it makes sense, released with a published advisory. Reporters who want credit are named once the fix has shipped. This project does not promise a reply time.

## Scope

In scope are the code in this repository and its GitHub Actions workflows.

Out of scope are other people's deployments of this project, findings that need physical access to the device or its power wiring, and reports that only describe missing hardening without a way to exploit it.

Do not test against a device or a service that you do not own.

## How this repository is checked

The owner triages the findings of the automated tooling in this repository with the same care as external reports. The tooling includes dependency review, gitleaks and OSSF Scorecard.

Some Scorecard alerts describe the limits of a repository with one owner, such as a `main` branch that needs no approving review, or a project that is young. Those alerts stay open and are not dismissed. The `main` branch still needs a pull request, the required checks, resolved review threads and verified signatures before anything is merged.
