# Security

ConflictBench operates with your user account's filesystem access. Its safety controls are aimed at accidental changes and interrupted operations, not at hostile writers or an attacker who can modify the selected folder, backup directory, or receipt. Pause all writers before mutation. Use a private local backup directory outside all sync roots.

Do not run as administrator/root. Do not supply untrusted executable diff tools. Do not open somebody else's receipt as if it were a trusted restore instruction. Do not edit a receipt to force rollback.

Report security concerns using GitHub's private vulnerability reporting if enabled, or open a minimal issue that contains no exploit payloads, private file contents or credentials and request a private channel. There is no paid support or guaranteed response-time policy.
