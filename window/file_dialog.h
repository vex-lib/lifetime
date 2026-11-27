#ifndef LIFETIME_WINDOW_FILE_DIALOG_H
#define LIFETIME_WINDOW_FILE_DIALOG_H

/**
 * window/file_dialog.h -- native OS modal Open/Save file panel bridge.
 */

#include "vex.h"

/**
 * Opens the host OS native file-selection panel (e.g. NSOpenPanel on macOS).
 * Guards against headless execution when no OS application loop is active.
 * @param title     dialog prompt title (may be nullptr)
 * @param filterExt comma-separated file extension filter (e.g. "png,jpg", may be nullptr)
 * @param outPath   destination buffer receiving UTF-8 file path
 * @param cap       capacity of outPath in bytes
 * @return true if a file path was selected and written to outPath
 */
bool OsFileDialog_open(string title, string filterExt, utf8 *outPath, bytes cap);

/**
 * Opens the host OS native file-save panel (e.g. NSSavePanel on macOS).
 * Guards against headless execution when no OS application loop is active.
 * @param title       dialog prompt title (may be nullptr)
 * @param defaultName suggested filename (may be nullptr)
 * @param filterExt   comma-separated file extension filter (may be nullptr)
 * @param outPath     destination buffer receiving UTF-8 file path
 * @param cap         capacity of outPath in bytes
 * @return true if a target path was chosen and written to outPath
 */
bool OsFileDialog_save(string title, string defaultName, string filterExt,
                       utf8 *outPath, bytes cap);

#endif
