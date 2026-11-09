# The update bot reads this file for the images that the workflows run with docker run, because it does not read run steps.
# Nothing builds it. The workflows read the image of a stage from this file, so a bump here is the bump the jobs use.
FROM ghcr.io/zizmorcore/zizmor:1.32.0@sha256:3014cc8f68f663935f69b3c3e0e41f5ce102b268d97db13db0f62208747f860d AS zizmor
FROM ghcr.io/gitleaks/gitleaks:v8.30.1@sha256:c00b6bd0aeb3071cbcb79009cb16a60dd9e0a7c60e2be9ab65d25e6bc8abbb7f AS gitleaks
