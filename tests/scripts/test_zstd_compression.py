import shutil

import pytest
from afni_test_utils import tools

# Define Data
data_paths = {"anatomical": "mini_data/anat_3mm.nii.gz"}


def brick_max(data, dset):
    """Return the maximum voxel value of dset, as a string."""
    stdout_log, _ = tools.run_cmd(data, f"3dBrickStat -slow -max {dset}")
    return stdout_log.read_text().strip()


def assert_same_data(data, dset_a, dset_b):
    """The two datasets must hold identical values."""
    diff = data.outdir / "diff+orig"
    cmd = f"3dcalc -overwrite -a {dset_a} -b {dset_b} -expr 'abs(a-b)' -prefix {diff}"
    tools.run_cmd(data, cmd, workdir=data.outdir)
    assert brick_max(data, f"{diff}.HEAD") == "0"


def test_nifti_zstd_roundtrip(data):
    """.nii.zst is written, read back, and holds the original values."""
    zst = data.outdir / "copied.nii.zst"
    tools.run_cmd(data, f"3dcopy -overwrite {data.anatomical} {zst}")
    assert zst.exists()
    assert_same_data(data, data.anatomical, zst)


def test_nifti_zstd_is_a_zstd_file(data):
    """Other software must be able to read what AFNI writes."""
    if not shutil.which("zstd"):
        pytest.skip("zstd program not installed")

    zst = data.outdir / "external.nii.zst"
    plain = data.outdir / "external.nii"
    tools.run_cmd(data, f"3dcopy -overwrite {data.anatomical} {zst}")
    tools.run_cmd(data, f"zstd -d -q -f {zst} -o {plain}", workdir=data.outdir)
    assert_same_data(data, data.anatomical, plain)


def test_zstd_file_from_zstd_program(data):
    """AFNI must be able to read what other software writes."""
    if not shutil.which("zstd"):
        pytest.skip("zstd program not installed")

    plain = data.outdir / "made_outside.nii"
    zst = data.outdir / "made_outside.nii.zst"
    tools.run_cmd(data, f"3dcopy -overwrite {data.anatomical} {plain}")
    tools.run_cmd(data, f"zstd -q -f {plain} -o {zst}", workdir=data.outdir)
    assert_same_data(data, plain, zst)


def test_nifti_zstd_sibling_is_not_confused(data):
    """foo.nii.zst must be read even when foo.nii.gz sits beside it."""
    zst = data.outdir / "sibling.nii.zst"
    gz = data.outdir / "sibling.nii.gz"
    tools.run_cmd(data, f"3dcopy -overwrite {data.anatomical} {zst}")
    cmd = f"3dcalc -overwrite -a {data.anatomical} -expr 'a+1' -prefix {gz}"
    tools.run_cmd(data, cmd, workdir=data.outdir)
    assert_same_data(data, data.anatomical, zst)


def test_brik_zstd_roundtrip(data):
    """AFNI_COMPRESSOR=ZSTD writes a .BRIK.zst that reads back unchanged."""
    out = data.outdir / "compressed+orig"
    cmd = f"3dcopy -overwrite {data.anatomical} {out}"
    tools.run_cmd(data, cmd, add_env_vars={"AFNI_COMPRESSOR": "ZSTD"})
    assert (data.outdir / "compressed+orig.BRIK.zst").exists()
    assert_same_data(data, data.anatomical, f"{out}.HEAD")


def test_zstd_level_changes_size(data):
    """AFNI_ZSTD_LEVEL is honored: a higher level gives a smaller file."""
    sizes = {}
    for level in ("1", "19"):
        dset = data.outdir / f"level{level}.nii.zst"
        cmd = f"3dcopy -overwrite {data.anatomical} {dset}"
        tools.run_cmd(data, cmd, add_env_vars={"AFNI_ZSTD_LEVEL": level})
        assert_same_data(data, data.anatomical, dset)
        sizes[level] = dset.stat().st_size
    assert sizes["19"] < sizes["1"]
