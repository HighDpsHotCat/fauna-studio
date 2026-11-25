from pathlib import Path


def cached_asset(repo, filename, progress):
    from huggingface_hub import hf_hub_download
    from huggingface_hub.errors import LocalEntryNotFoundError

    name, kind, revision = repo
    args = dict(repo_id=name, repo_type=kind, revision=revision, filename=filename)
    try:
        return Path(hf_hub_download(**args, local_files_only=True))
    except LocalEntryNotFoundError:
        progress(f"최초 다운로드 중 · {Path(filename).name}")
        return Path(hf_hub_download(**args))
