"""R2 credential admission for the shared CI cache bucket, without R2 I/O.

The Bazel remote-cache proxy and the CI cache probe consume these admission
contracts. The credentials stay in the environment instead of process
arguments so they never reach logs; secretless runs (fork PRs, Dependabot)
see none of them and get no credentials object at all. The bucket stays the
literal shared identity: no rename, no discovery, no other bucket.
"""

import unittest

from ao.core import r2_credentials

COMPLETE = {
    "R2_ENDPOINT": "https://account.r2.cloudflarestorage.com",
    "R2_ACCESS_KEY_ID": "key-id",
    "R2_SECRET_ACCESS_KEY": "secret-key",
}


class R2CredentialsAdmissionTest(unittest.TestCase):
    def test_every_credential_is_required(self):
        for missing in COMPLETE:
            with self.subTest(missing=missing):
                environ = {key: value for key, value in COMPLETE.items() if key != missing}
                self.assertIsNone(r2_credentials.r2_credentials(environ))
        self.assertIsNone(r2_credentials.r2_credentials({}))

    def test_empty_credential_values_are_absent_not_admitted(self):
        # The admission is field-symmetric all(value.strip()); one field x
        # one whitespace blank represents every identical-outcome cell.
        self.assertIsNone(r2_credentials.r2_credentials(COMPLETE | {"R2_ACCESS_KEY_ID": " "}))

    def test_complete_credentials_are_admitted_without_extra_env_requirements(self):
        credentials = r2_credentials.r2_credentials(COMPLETE | {"R2_OTHER_SETTING": "ignored"})
        self.assertEqual(
            (credentials.endpoint, credentials.access_key_id, credentials.secret_access_key),
            (COMPLETE["R2_ENDPOINT"], COMPLETE["R2_ACCESS_KEY_ID"], COMPLETE["R2_SECRET_ACCESS_KEY"]),
        )


if __name__ == "__main__":
    unittest.main()
