"""
PIO extra_script: wrap dfu-util so benign STM32H7 exit 74 doesn't look like
a failure in the VSCode upload button / pio run output.
"""
Import("env")  # noqa: F821

from os.path import join

if env.subst("$UPLOAD_PROTOCOL") == "dfu":
    wrapper = join(env.subst("$PROJECT_DIR"), "extra", "dfu_tolerant.py")
    env.Replace(
        UPLOADCMD='"$PYTHONEXE" "' + wrapper
        + '" $UPLOADER $UPLOADERFLAGS "${SOURCE.get_abspath()}"'
    )
