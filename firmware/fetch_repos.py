import os
import subprocess
import json


def run_logged(args):
    result = subprocess.run(
        args,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        raise subprocess.CalledProcessError(
            result.returncode, args, output=result.stdout
        )
    return result


def get_tracked_status(path):
    result = subprocess.run(
        ["git", "-C", path, "status", "--porcelain", "--untracked-files=no"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        raise subprocess.CalledProcessError(
            result.returncode, result.args, output=result.stdout
        )
    return [line for line in result.stdout.splitlines() if line.strip()]


def restore_tracked_files(path, ref):
    # Some dependency repos end up with tracked files deleted locally even when
    # HEAD already points at the target ref. Force restoring tracked content.
    run_logged(["git", "-C", path, "checkout", "-f", ref, "--", "."])


def clone_or_update_repo(
    repo_url, path, ref=None, with_submodules=False, patch_path=None
):
    import os

    if not os.path.exists(path):
        run_logged(["git", "clone", repo_url, path])
    else:
        run_logged(["git", "-C", path, "fetch"])

    if ref:
        run_logged(["git", "-C", path, "checkout", ref])
        tracked_status = get_tracked_status(path)
        if tracked_status:
            restore_tracked_files(path, ref)
            tracked_status = get_tracked_status(path)
            if tracked_status:
                raise RuntimeError(
                    f"Repository {path} still has tracked local changes after restore"
                )

    if with_submodules:
        run_logged(
            ["git", "-C", path, "submodule", "update", "--init", "--recursive"]
        )

    # 应用 patch
    if patch_path:
        patch_full_path = (
            patch_path
            if os.path.isabs(patch_path)
            else os.path.join(os.getcwd(), patch_path)
        )
        # 使用 git apply --check 先检测补丁是否能应用，避免报错
        check_result = subprocess.run(
            ["git", "-C", path, "apply", "--check", patch_full_path],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if check_result.stdout:
            print(check_result.stdout, end="")
        if check_result.returncode == 0:
            run_logged(["git", "-C", path, "apply", patch_full_path])
            print(f"Applied patch {patch_path} to {path}")
        else:
            print(f"Patch {patch_path} cannot be applied cleanly to {path}, skipped.")


def fetch_dependencies():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "repos.json")

    with open(config_path) as f:
        repos = json.load(f)

    for repo in repos:
        repo_path = os.path.join(script_dir, repo["path"])
        branch = repo.get("branch")
        with_submodules = repo.get("with_submodules", False)
        patch = repo.get("patch")
        if patch and not os.path.isabs(patch):
            patch = os.path.join(script_dir, patch)
        clone_or_update_repo(repo["url"], repo_path, branch, with_submodules, patch)


if __name__ == "__main__":
    fetch_dependencies()
