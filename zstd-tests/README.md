# zstd test harness (not part of the PR)

Scratch tests for the `zstd` branch. This branch is deliberately separate so
these files never end up in the pull request.

- `ztest.c`  — direct znzlib stress test: ragged writes, random seeks, header
  reads, EOF, SEEK_END, throughput.  Build against the branch's znzlib:

      gcc -O2 -std=gnu99 -DHAVE_ZLIB -DHAVE_ZSTD -I<afni>/src/nifti/znzlib \
          ztest.c <afni>/src/nifti/znzlib/znzlib.c -o ztest -lzstd -lz -lpthread

      ./ztest write out.zst ref.bin    # write ref.bin through znzlib
      ./ztest check out.zst ref.bin    # verify (PASS/FAIL)
      ./ztest bench out.zst ref.bin    # sequential 4MB "brick" reads

  Build a second copy with `-fsanitize=address,undefined` and run `check`
  under it; and one with `-DZNZ_ZSTD_NO_THREADS` for the single-threaded path.

- `e2e.sh <afni bin dir> <input.nii.gz> <workdir>` — end-to-end tests against a
  built AFNI: round trips, zstd/pzstd interoperability, header-only read size,
  3drefit, the .nii.gz/.nii.zst sibling case, read speed, and levels 1/3/6.

- `gui.sh <afni bin dir> <dataset> <workdir>` — drives the AFNI GUI headlessly
  (Xvfb, so Linux only) with two .nii.zst datasets in one session.

Use a real 4D dataset (a GB or so). Bugs in the frame index only appear past
64 frames, which is about 512 MB at the default frame size.
