"""Step-local credentials shared by the governed R2 cache clients."""

from collections.abc import Mapping
from dataclasses import dataclass

# Existing remote storage identity; distinct clients retain their own prefixes.
R2_BUCKET = "aobus-sccache"


@dataclass(frozen=True)
class R2Credentials:
    endpoint: str
    access_key_id: str
    secret_access_key: str


def r2_credentials(environ: Mapping[str, str]) -> R2Credentials | None:
    """Return the step-local R2 secrets, or None unless every one is present.

    CI binds the R2 secrets to the step-local R2_ENDPOINT, R2_ACCESS_KEY_ID,
    and R2_SECRET_ACCESS_KEY names; secretless runs (fork PRs, Dependabot) see
    none of them. The credentials stay in the environment instead of process
    arguments so they never reach logs.
    """
    credentials = R2Credentials(
        environ.get("R2_ENDPOINT", ""), environ.get("R2_ACCESS_KEY_ID", ""), environ.get("R2_SECRET_ACCESS_KEY", "")
    )
    return credentials if all(value.strip() for value in vars(credentials).values()) else None
