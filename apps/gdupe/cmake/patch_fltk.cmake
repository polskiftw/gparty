# gdupe-specific surgical patching for the exact pinned FLTK revision.
#
# FLTK's Windows driver objects implement optional text-input, text-editor, and
# file-browser hooks in the same translation units as platform services gdupe
# actually needs. Because those virtual/provider references make otherwise
# unused widget subtrees linkable, gdupe removes only the hooks it cannot use.
# Every replacement is exact-match guarded so a future FLTK pin change fails
# configuration rather than silently applying a stale patch.

function(gdupe_replace_exact path old_text new_text description)
  file(READ "${path}" contents)
  string(FIND "${contents}" "${old_text}" match_at)
  if(match_at EQUAL -1)
    message(FATAL_ERROR "Pinned FLTK patch no longer matches: ${description}")
  endif()
  string(REPLACE "${old_text}" "${new_text}" contents "${contents}")
  file(WRITE "${path}" "${contents}")
endfunction()

set(screen_driver "${fltk_SOURCE_DIR}/src/Fl_Screen_Driver.cxx")
set(win_screen_driver "${fltk_SOURCE_DIR}/src/drivers/WinAPI/Fl_WinAPI_Screen_Driver.cxx")
set(win_system_driver "${fltk_SOURCE_DIR}/src/drivers/WinAPI/Fl_WinAPI_System_Driver.cxx")

set(old_input_handler [=[int Fl_Screen_Driver::input_widget_handle_key(int key, unsigned mods, unsigned shift, Fl_Input *input)
{
  switch (key) {
    case FL_Delete: {
      int selected = (input->insert_position() != input->mark()) ? 1 : 0;
      if (mods==0 && shift && selected)
        return input->kf_copy_cut();            // Shift-Delete with selection (WP,NP,WOW,GE,KE,OF)
      if (mods==0 && shift && !selected)
        return input->kf_delete_char_right();   // Shift-Delete no selection (WP,NP,WOW,GE,KE,!OF)
      if (mods==0)          return input->kf_delete_char_right();       // Delete         (Standard)
      if (mods==FL_CTRL)    return input->kf_delete_word_right();       // Ctrl-Delete    (WP,!NP,WOW,GE,KE,!OF)
      return 0;                                                 // ignore other combos, pass to parent
    }

    case FL_Left:
      if (mods==0)          return input->kf_move_char_left();          // Left           (WP,NP,WOW,GE,KE,OF)
      if (mods==FL_CTRL)    return input->kf_move_word_left();          // Ctrl-Left      (WP,NP,WOW,GE,KE,!OF)
      if (mods==FL_META)    return input->kf_move_char_left();          // Meta-Left      (WP,NP,?WOW,GE,KE)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_Right:
      if (mods==0)          return input->kf_move_char_right(); // Right          (WP,NP,WOW,GE,KE,OF)
      if (mods==FL_CTRL)    return input->kf_move_word_right(); // Ctrl-Right     (WP,NP,WOW,GE,KE,!OF)
      if (mods==FL_META)    return input->kf_move_char_right(); // Meta-Right     (WP,NP,?WOW,GE,KE,!OF)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_Up:
      if (mods==0)          return input->kf_lines_up(1);               // Up             (WP,NP,WOW,GE,KE,OF)
      if (mods==FL_CTRL)    return input->kf_move_up_and_sol(); // Ctrl-Up        (WP,!NP,WOW,GE,!KE,OF)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_Down:
      if (mods==0)          return input->kf_lines_down(1);             // Dn             (WP,NP,WOW,GE,KE,OF)
      if (mods==FL_CTRL)    return input->kf_move_down_and_eol();       // Ctrl-Down      (WP,!NP,WOW,GE,!KE,OF)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_Page_Up:
      // Fl_Input has no scroll control, so instead we move the cursor by one page
      if (mods==0)          return input->kf_page_up();         // PageUp         (WP,NP,WOW,GE,KE)
      if (mods==FL_CTRL)    return input->kf_page_up();         // Ctrl-PageUp    (!WP,!NP,!WOW,!GE,KE,OF)
      if (mods==FL_ALT)     return input->kf_page_up();         // Alt-PageUp     (!WP,!NP,!WOW,!GE,KE,OF)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_Page_Down:
      if (mods==0)          return input->kf_page_down();               // PageDn         (WP,NP,WOW,GE,KE)
      if (mods==FL_CTRL)    return input->kf_page_down();               // Ctrl-PageDn    (!WP,!NP,!WOW,!GE,KE,OF)
      if (mods==FL_ALT)     return input->kf_page_down();               // Alt-PageDn     (!WP,!NP,!WOW,!GE,KE,OF)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_Home:
      if (mods==0)          return input->kf_move_sol();                // Home           (WP,NP,WOW,GE,KE,OF)
      if (mods==FL_CTRL)    return input->kf_top();                     // Ctrl-Home      (WP,NP,WOW,GE,KE,OF)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_End:
      if (mods==0)          return input->kf_move_eol();                // End            (WP,NP,WOW,GE,KE,OF)
      if (mods==FL_CTRL)    return input->kf_bottom();                  // Ctrl-End       (WP,NP,WOW,GE,KE,OF)
      return 0;                                                 // ignore other combos, pass to parent

    case FL_BackSpace:
      if (mods==0)          return input->kf_delete_char_left();        // Backspace      (WP,NP,WOW,GE,KE,OF)
      if (mods==FL_CTRL)    return input->kf_delete_word_left();        // Ctrl-Backspace (WP,!NP,WOW,GE,KE,!OF)
      return 0;
      // ignore other combos, pass to parent
  }
  return -1;
}]=])
set(new_input_handler [=[int Fl_Screen_Driver::input_widget_handle_key(int, unsigned, unsigned, Fl_Input *)
{
  // gdupe has no editable text widgets.
  return -1;
}]=])
gdupe_replace_exact("${screen_driver}" "${old_input_handler}" "${new_input_handler}"
  "remove unused Fl_Input keyboard provider")

set(old_text_editor_binding [=[static Fl_Text_Editor::Key_Binding extra_bindings[] =  {
  // Define Windows specific accelerators...
  { 'y',          FL_CTRL,                  Fl_Text_Editor::kf_redo       ,0},
  { 0,            0,                        0                             ,0}
};


Fl_WinAPI_Screen_Driver::Fl_WinAPI_Screen_Driver() : Fl_Screen_Driver() {
  text_editor_extra_key_bindings =  extra_bindings;]=])
set(new_text_editor_binding [=[Fl_WinAPI_Screen_Driver::Fl_WinAPI_Screen_Driver() : Fl_Screen_Driver() {]=])
gdupe_replace_exact("${win_screen_driver}" "${old_text_editor_binding}" "${new_text_editor_binding}"
  "remove unused Windows Fl_Text_Editor Ctrl+Y binding")

set(old_file_browser [=[int Fl_WinAPI_System_Driver::file_browser_load_filesystem(Fl_File_Browser *browser, char *filename,
                                                          int lname, Fl_File_Icon *icon) {
  int num_files = 0;
# ifdef __CYGWIN__
  //
  // Cygwin provides an implementation of setmntent() to get the list
  // of available drives...
  //
  FILE          *m = setmntent("/-not-used-", "r");
  struct mntent *p;
  while ((p = getmntent (m)) != NULL) {
    browser->add(p->mnt_dir, icon);
    num_files ++;
  }
  endmntent(m);
# else
  //
  // Normal Windows code uses drive bits...
  //
  DWORD drives;         // Drive available bits
  drives = GetLogicalDrives();
  for (int i = 'A'; i <= 'Z'; i ++, drives >>= 1) {
    if (drives & 1) {
      snprintf(filename, lname, "%c:/", i);
      if (i < 'C') // see also: GetDriveType and GetVolumeInformation in Windows
        browser->add(filename, icon);
      else
        browser->add(filename, icon);
      num_files ++;
    }
  }
# endif // __CYGWIN__
  return num_files;
}]=])
set(new_file_browser [=[int Fl_WinAPI_System_Driver::file_browser_load_filesystem(Fl_File_Browser *, char *,
                                                          int, Fl_File_Icon *) {
  // gdupe has no file browser or file chooser UI.
  return 0;
}]=])
gdupe_replace_exact("${win_system_driver}" "${old_file_browser}" "${new_file_browser}"
  "remove unused Windows Fl_File_Browser drive enumerator")
