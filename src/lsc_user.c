/* GVM
 * $Id$
 * Description: LSC user credentials package generation.
 *
 * Authors:
 * Matthew Mundell <matthew.mundell@greenbone.net>
 * Michael Wiegand <michael.wiegand@greenbone.net>
 * Felix Wolfsteller <felix.wolfsteller@greenbone.net>
 *
 * Copyright:
 * Copyright (C) 2009 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gvm/util/fileutils.h>

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "md manage"


/* Key creation. */

/**
 * @brief Create an ssh key for local security checks.
 *
 * Forks and creates a key for local checks by calling
 * 'ssh-keygen -t rsa -f filepath -C "comment" -P "passhprase"'.
 * A directory will be created if it does not exist.
 *
 * @param[in]  comment     Comment to use.
 * @param[in]  passphrase  Passphrase for key, must be longer than 4 characters.
 * @param[in]  privpath    Filename of the key file.
 *
 * @return 0 if successful, -1 otherwise.
 */
static int
create_ssh_key (const char *comment, const char *passphrase,
                const char *privpath)
{
  gchar *astdout = NULL;
  gchar *astderr = NULL;
  GError *err = NULL;
  gint exit_status = 0;
  gchar *dir;
  char *command;

  /* Sanity-check essential parameters. */

  if (!comment || comment[0] == '\0')
    {
      g_debug ("%s: comment must be set", __FUNCTION__);
      return -1;
    }
  if (!passphrase || strlen (passphrase) < 5)
    {
      g_debug ("%s: password must be longer than 4 characters", __FUNCTION__);
      return -1;
    }

  /* Sanity check files. */

  dir = g_path_get_dirname (privpath);
  if (g_mkdir_with_parents (dir, 0755 /* "rwxr-xr-x" */ ))
    {
      g_debug ("%s: failed to access %s", __FUNCTION__, dir);
      g_free (dir);
      return -1;
    }
  g_free (dir);

  /* Spawn ssh-keygen. */
  command = g_strconcat ("ssh-keygen -t rsa -f ", privpath, " -C \"", comment,
                         "\" -P \"", passphrase, "\"", NULL);
  g_debug ("command: ssh-keygen -t rsa -f %s -C \"%s\" -P \"********\"",
           privpath, comment);

  if ((g_spawn_command_line_sync (command, &astdout, &astderr, &exit_status,
                                  &err)
       == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      if (err)
        {
          g_debug ("%s: failed to create private key: %s\n",
                   __FUNCTION__, err->message);
          g_error_free (err);
        }
      else
        g_debug ("%s: failed to create private key\n", __FUNCTION__);
      g_debug ("%s: key-gen failed with %d (WIF %i, WEX %i).\n",
               __FUNCTION__, exit_status, WIFEXITED (exit_status),
               WEXITSTATUS (exit_status));
      g_debug ("%s: stdout: %s", __FUNCTION__, astdout);
      g_debug ("%s: stderr: %s", __FUNCTION__, astderr);
      g_free (command);
      g_free (astdout);
      g_free (astderr);
      return -1;
    }
  g_free (command);
  g_free (astdout);
  g_free (astderr);
  return 0;
}

/**
 * @brief Create local security check (LSC) keys.
 *
 * @param[in]   password     Password.
 * @param[out]  private_key  Private key.
 *
 * @return 0 success, -1 error.
 */
int
lsc_user_keys_create (const gchar *password,
                      gchar **private_key)
{
  GError *error;
  gsize length;
  char key_dir[] = "/tmp/openvas_key_XXXXXX";
  gchar *key_path = NULL;
  int ret = -1;

  /* Make a directory for the keys. */

  if (mkdtemp (key_dir) == NULL)
    return -1;

  /* Create private key. */
  key_path = g_build_filename (key_dir, "key", NULL);
  if (create_ssh_key ("Key generated by GVM", password, key_path))
    goto free_exit;

  error = NULL;
  g_file_get_contents (key_path, private_key, &length, &error);
  if (error)
    {
      g_error_free (error);
      goto free_exit;
    }
  ret = 0;
 free_exit:

  g_free (key_path);
  gvm_file_remove_recurse (key_dir);
  return ret;
}


/* RPM package generation. */

/**
 * @brief Attempts creation of RPM packages to create a user and install a
 * @brief public key file for it.
 *
 * @param[in]  username         Name of user.
 * @param[in]  public_key_path  Location of public key.
 * @param[in]  to_filename      Destination filename for RPM.
 *
 * @return Path to rpm file if successful, NULL otherwise.
 */
static gboolean
lsc_user_rpm_create (const gchar *username,
                     const gchar *public_key_path,
                     const gchar *to_filename)
{
  gint exit_status;
  gchar *new_pubkey_filename = NULL;
  gchar *pubkey_basename = NULL;
  gchar **cmd;
  char tmpdir[] = "/tmp/lsc_user_rpm_create_XXXXXX";
  gboolean success = TRUE;
  gchar *standard_out = NULL;
  gchar *standard_err = NULL;

  /* Create a temporary directory. */

  g_debug ("%s: create temporary directory", __FUNCTION__);
  if (mkdtemp (tmpdir) == NULL)
    return FALSE;
  g_debug ("%s: temporary directory: %s\n", __FUNCTION__, tmpdir);

  /* Copy the public key into the temporary directory. */

  g_debug ("%s: copy key to temporary directory\n", __FUNCTION__);
  pubkey_basename = g_strdup_printf ("%s.pub", username);
  new_pubkey_filename = g_build_filename (tmpdir, pubkey_basename, NULL);
  if (gvm_file_copy (public_key_path, new_pubkey_filename)
      == FALSE)
    {
      g_debug ("%s: failed to copy key file %s to %s",
               __FUNCTION__, public_key_path, new_pubkey_filename);
      g_free (pubkey_basename);
      g_free (new_pubkey_filename);
      return FALSE;
    }

  /* Execute create-rpm script with the temporary directory as the
   * target and the public key in the temporary directory as the key. */

  g_debug ("%s: Attempting RPM build\n", __FUNCTION__);
  cmd = (gchar **) g_malloc (6 * sizeof (gchar *));
  cmd[0] = g_build_filename (GVM_DATA_DIR,
                             "gvm-lsc-rpm-creator.sh",
                             NULL);
  cmd[1] = g_strdup (username);
  cmd[2] = g_strdup (new_pubkey_filename);
  cmd[3] = g_strdup (tmpdir);
  cmd[4] = g_strdup (to_filename);
  cmd[5] = NULL;
  g_debug ("%s: Spawning in %s: %s %s %s %s %s\n",
           __FUNCTION__, tmpdir, cmd[0], cmd[1], cmd[2], cmd[3], cmd[4]);
  if ((g_spawn_sync (tmpdir,
                     cmd,
                     NULL,                  /* Environment. */
                     G_SPAWN_SEARCH_PATH,
                     NULL,                  /* Setup function. */
                     NULL,
                     &standard_out,
                     &standard_err,
                     &exit_status,
                     NULL)
       == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      g_debug ("%s: failed to create the rpm: %d (WIF %i, WEX %i)",
               __FUNCTION__,
               exit_status,
               WIFEXITED (exit_status),
               WEXITSTATUS (exit_status));
      g_debug ("%s: stdout: %s\n", __FUNCTION__, standard_out);
      g_debug ("%s: stderr: %s\n", __FUNCTION__, standard_err);
      success = FALSE;
    }

  g_free (cmd[0]);
  g_free (cmd[1]);
  g_free (cmd[2]);
  g_free (cmd[3]);
  g_free (cmd[4]);
  g_free (cmd);
  g_free (pubkey_basename);
  g_free (new_pubkey_filename);
  g_free (standard_out);
  g_free (standard_err);

  /* Remove the copy of the public key and the temporary directory. */

  if (gvm_file_remove_recurse (tmpdir) != 0 && success == TRUE)
    {
      g_debug ("%s: failed to remove temporary directory %s",
               __FUNCTION__, tmpdir);
      success = FALSE;
    }

  return success;
}

/**
 * @brief Recreate RPM package.
 *
 * @param[in]   name         User name.
 * @param[in]   public_key   Public key.
 * @param[out]  rpm          RPM package.
 * @param[out]  rpm_size     Size of RPM package, in bytes.
 *
 * @return 0 success, -1 error.
 */
int
lsc_user_rpm_recreate (const gchar *name, const char *public_key,
                       void **rpm, gsize *rpm_size)
{
  GError *error;
  char rpm_dir[] = "/tmp/rpm_XXXXXX";
  char key_dir[] = "/tmp/key_XXXXXX";
  gchar *rpm_path, *public_key_path;
  int ret = -1;

  /* Make a directory for the key. */

  if (mkdtemp (key_dir) == NULL)
    return -1;

  /* Write public key to file. */

  error = NULL;
  public_key_path = g_build_filename (key_dir, "key.pub", NULL);
  g_file_set_contents (public_key_path, public_key, strlen (public_key),
                       &error);
  if (error)
    goto free_exit;

  /* Create RPM package. */

  if (mkdtemp (rpm_dir) == NULL)
    goto free_exit;
  rpm_path = g_build_filename (rpm_dir, "p.rpm", NULL);
  g_debug ("%s: rpm_path: %s", __FUNCTION__, rpm_path);
  if (lsc_user_rpm_create (name, public_key_path, rpm_path) == FALSE)
    {
      g_free (rpm_path);
      goto rm_exit;
    }

  /* Read the package into memory. */

  error = NULL;
  g_file_get_contents (rpm_path, (gchar **) rpm, rpm_size, &error);
  g_free (rpm_path);
  if (error)
    {
      g_error_free (error);
      goto rm_exit;
    }

  /* Return. */

  ret = 0;

 rm_exit:

  gvm_file_remove_recurse (rpm_dir);

 free_exit:

  g_free (public_key_path);

  gvm_file_remove_recurse (key_dir);

  return ret;
}


/* Deb generation. */

/**
 * @brief Attempts creation of Debian packages to create a user and install a
 * @brief public key file for it.
 *
 * @param[in]  username         Name of user.
 * @param[in]  public_key_path  Location of public key.
 * @param[in]  to_filename      Destination filename for RPM.
 * @param[in]  maintainer       Maintainer email address.
 *
 * @return Path to rpm file if successful, NULL otherwise.
 */
static gboolean
lsc_user_deb_create (const gchar *username,
                     const gchar *public_key_path,
                     const gchar *to_filename,
                     const gchar *maintainer)
{
  gint exit_status;
  gchar *new_pubkey_filename = NULL;
  gchar *pubkey_basename = NULL;
  gchar **cmd;
  char tmpdir[] = "/tmp/lsc_user_deb_create_XXXXXX";
  gboolean success = TRUE;
  gchar *standard_out = NULL;
  gchar *standard_err = NULL;

  /* Create a temporary directory. */

  g_debug ("%s: create temporary directory", __FUNCTION__);
  if (mkdtemp (tmpdir) == NULL)
    return FALSE;
  g_debug ("%s: temporary directory: %s\n", __FUNCTION__, tmpdir);

  /* Copy the public key into the temporary directory. */

  g_debug ("%s: copy key to temporary directory\n", __FUNCTION__);
  pubkey_basename = g_strdup_printf ("%s.pub", username);
  new_pubkey_filename = g_build_filename (tmpdir, pubkey_basename, NULL);
  if (gvm_file_copy (public_key_path, new_pubkey_filename)
      == FALSE)
    {
      g_debug ("%s: failed to copy key file %s to %s",
               __FUNCTION__, public_key_path, new_pubkey_filename);
      g_free (pubkey_basename);
      g_free (new_pubkey_filename);
      return FALSE;
    }

  /* Execute create-deb script with the temporary directory as the
   * target and the public key in the temporary directory as the key. */

  g_debug ("%s: Attempting DEB build\n", __FUNCTION__);
  cmd = (gchar **) g_malloc (7 * sizeof (gchar *));
  cmd[0] = g_build_filename (GVM_DATA_DIR,
                             "gvm-lsc-deb-creator.sh",
                             NULL);
  cmd[1] = g_strdup (username);
  cmd[2] = g_strdup (new_pubkey_filename);
  cmd[3] = g_strdup (tmpdir);
  cmd[4] = g_strdup (to_filename);
  cmd[5] = g_strdup (maintainer);
  cmd[6] = NULL;
  g_debug ("%s: Spawning in %s: %s %s %s %s %s %s\n",
           __FUNCTION__, tmpdir,
           cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5]);
  if ((g_spawn_sync (tmpdir,
                     cmd,
                     NULL,                  /* Environment. */
                     G_SPAWN_SEARCH_PATH,
                     NULL,                  /* Setup function. */
                     NULL,
                     &standard_out,
                     &standard_err,
                     &exit_status,
                     NULL)
       == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      g_debug ("%s: failed to create the deb: %d (WIF %i, WEX %i)",
               __FUNCTION__,
               exit_status,
               WIFEXITED (exit_status),
               WEXITSTATUS (exit_status));
      g_debug ("%s: stdout: %s\n", __FUNCTION__, standard_out);
      g_debug ("%s: stderr: %s\n", __FUNCTION__, standard_err);
      success = FALSE;
    }

  g_free (cmd[0]);
  g_free (cmd[1]);
  g_free (cmd[2]);
  g_free (cmd[3]);
  g_free (cmd[4]);
  g_free (cmd[5]);
  g_free (cmd);
  g_free (pubkey_basename);
  g_free (new_pubkey_filename);
  g_free (standard_out);
  g_free (standard_err);

  /* Remove the copy of the public key and the temporary directory. */

  if (gvm_file_remove_recurse (tmpdir) != 0 && success == TRUE)
    {
      g_debug ("%s: failed to remove temporary directory %s",
               __FUNCTION__, tmpdir);
      success = FALSE;
    }

  return success;
}

/**
 * @brief Recreate DEB package.
 *
 * @param[in]   name         User name.
 * @param[in]   public_key   Public key.
 * @param[in]   maintainer   The maintainer email address.
 * @param[out]  deb          DEB package.
 * @param[out]  deb_size     Size of DEB package, in bytes.
 *
 * @return 0 success, -1 error.
 */
int
lsc_user_deb_recreate (const gchar *name, const char *public_key,
                       const char *maintainer,
                       void **deb, gsize *deb_size)
{
  GError *error;
  char deb_dir[] = "/tmp/deb_XXXXXX";
  char key_dir[] = "/tmp/key_XXXXXX";
  gchar *deb_path, *public_key_path;
  int ret = -1;

  /* Make a directory for the key. */

  if (mkdtemp (key_dir) == NULL)
    return -1;

  /* Write public key to file. */

  error = NULL;
  public_key_path = g_build_filename (key_dir, "key.pub", NULL);
  g_file_set_contents (public_key_path, public_key, strlen (public_key),
                       &error);
  if (error)
    goto free_exit;

  /* Create DEB package. */

  if (mkdtemp (deb_dir) == NULL)
    goto free_exit;
  deb_path = g_build_filename (deb_dir, "p.deb", NULL);
  g_debug ("%s: deb_path: %s", __FUNCTION__, deb_path);
  if (lsc_user_deb_create (name, public_key_path, deb_path, maintainer)
        == FALSE)
    {
      g_free (deb_path);
      goto rm_exit;
    }

  /* Read the package into memory. */

  error = NULL;
  g_file_get_contents (deb_path, (gchar **) deb, deb_size, &error);
  g_free (deb_path);
  if (error)
    {
      g_error_free (error);
      goto rm_exit;
    }

  /* Return. */

  ret = 0;

 rm_exit:

  gvm_file_remove_recurse (deb_dir);

 free_exit:

  g_free (public_key_path);

  gvm_file_remove_recurse (key_dir);

  return ret;
}


/* Exe generation. */

/**
 * @brief Write an NSIS installer script to file.
 *
 * @param[in]  script_name   Name of script.
 * @param[in]  package_name  Name of package.
 * @param[in]  user_name     User name.
 * @param[in]  password      User password.
 *
 * @return 0 success, -1 error.
 */
static int
create_nsis_script (const gchar *script_name, const gchar *package_name,
                    const gchar *user_name, const gchar *password)
{
  FILE* fd;

  fd = fopen (script_name, "w");
  if (fd == NULL)
    return -1;

  // Write part about default section
  fprintf (fd, "#Installer filename\n");
  fprintf (fd, "outfile ");
  fprintf (fd, "%s", package_name);
  fprintf (fd, "\n\n");

  fprintf (fd, "# Set desktop as install directory\n");
  fprintf (fd, "installDir $DESKTOP\n\n");

  fprintf (fd, "# Put some text\n");
  fprintf (fd, "BrandingText \"GVM Local Security Checks User\"\n\n");

  // For ms vista installers we need the UAC plugin and use the following lines:
  // This requires the user to have the UAC plugin installed and to provide the
  // the path to it.
  //fprintf (fd, "# Request application privileges for Windows Vista\n");
  //fprintf (fd, "RequestExecutionLevel admin\n\n");

  fprintf (fd, "#\n# Default (installer) section.\n#\n");
  fprintf (fd, "section\n\n");

  fprintf (fd, "# Define output path\n");
  fprintf (fd, "setOutPath $INSTDIR\n\n");

  fprintf (fd, "# Uninstaller name\n");
  fprintf (fd, "writeUninstaller $INSTDIR\\openvas_lsc_remove_%s.exe\n\n",
           user_name);

  // Need to find localized Administrators group name, create a
  // GetAdminGroupName - vb script (Thanks to Thomas Rotter)
  fprintf (fd, "# Create Thomas Rotters GetAdminGroupName.vb script\n");
  fprintf (fd, "ExecWait \"cmd /C Echo Set objWMIService = GetObject($\\\"winmgmts:\\\\.\\root\\cimv2$\\\") > $\\\"%%temp%%\\GetAdminGroupName.vbs$\\\" \"\n");
  fprintf (fd, "ExecWait \"cmd /C Echo Set colAccounts = objWMIService.ExecQuery ($\\\"Select * From Win32_Group Where SID = 'S-1-5-32-544'$\\\")  >> $\\\"%%temp%%\\GetAdminGroupName.vbs$\\\"\"\n");
  fprintf (fd, "ExecWait \"cmd /C Echo For Each objAccount in colAccounts >> $\\\"%%temp%%\\GetAdminGroupName.vbs$\\\"\"\n");
  fprintf (fd, "ExecWait \"cmd /C Echo Wscript.Echo objAccount.Name >> $\\\"%%temp%%\\GetAdminGroupName.vbs$\\\"\"\n");
  fprintf (fd, "ExecWait \"cmd /C Echo Next >> $\\\"%%temp%%\\GetAdminGroupName.vbs$\\\"\"\n");
  fprintf (fd, "ExecWait \"cmd /C cscript //nologo $\\\"%%temp%%\\GetAdminGroupName.vbs$\\\" > $\\\"%%temp%%\\AdminGroupName.txt$\\\"\"\n\n");

  /** @todo provide /comment:"GVM User" /fullname:"GVM Testuser" */
  fprintf (fd, "# Create batch script that installs the user\n");
  fprintf (fd, "ExecWait \"cmd /C Echo Set /P AdminGroupName= ^<$\\\"%%temp%%\\AdminGroupName.txt$\\\" > $\\\"%%temp%%\\AddUser.bat$\\\"\" \n");
  fprintf (fd, "ExecWait \"cmd /C Echo net user %s %s /add /active:yes >> $\\\"%%temp%%\\AddUser.bat$\\\"\"\n",
           user_name,
           password);
  fprintf (fd, "ExecWait \"cmd /C Echo net localgroup %%AdminGroupName%% %%COMPUTERNAME%%\\%s /add >> $\\\"%%temp%%\\AddUser.bat$\\\"\"\n\n",
           user_name);

  fprintf (fd, "# Execute AddUser script\n");
  fprintf (fd, "ExecWait \"cmd /C $\\\"%%temp%%\\AddUser.bat$\\\"\"\n\n");

  // Remove up temporary files for localized Administrators group names
  fprintf (fd, "# Remove temporary files for localized admin group names\n");
  fprintf (fd, "ExecWait \"del $\\\"%%temp%%\\AdminGroupName.txt$\\\"\"\n");
  fprintf (fd, "ExecWait \"del $\\\"%%temp%%\\GetAdminGroupName.vbs$\\\"\"\n\n");
  fprintf (fd, "ExecWait \"del $\\\"%%temp%%\\AddUser.bat$\\\"\"\n\n");

  /** @todo Display note about NTLM and SMB signing and encryption, 'Easy Filesharing' in WIN XP */
  fprintf (fd, "# Display message that everything seems to be fine\n");
  fprintf (fd, "messageBox MB_OK \"A user has been added. An uninstaller is placed on your Desktop.\"\n\n");

  fprintf (fd, "# Default (install) section end\n");
  fprintf (fd, "sectionEnd\n\n");

  // Write part about uninstall section
  fprintf (fd, "#\n# Uninstaller section.\n#\n");
  fprintf (fd, "section \"Uninstall\"\n\n");

  fprintf (fd, "# Run cmd to remove user\n");
  fprintf (fd, "ExecWait \"net user %s /delete\"\n\n",
           user_name);

  /** @todo Uninstaller should remove itself */
  fprintf (fd, "# Unistaller should remove itself (from desktop/installdir)\n\n");

  fprintf (fd, "# Display message that everything seems to be fine\n");
  fprintf (fd, "messageBox MB_OK \"A user has been removed. You can now safely remove the uninstaller from your Desktop.\"\n\n");

  fprintf (fd, "# Uninstaller section end\n");
  fprintf (fd, "sectionEnd\n\n");

  if (fclose (fd))
    return -1;

  return 0;
}

/**
 * @brief Execute makensis to create a package from an NSIS script.
 *
 * Run makensis in the directory that nsis_script is in.
 *
 * @param[in]  nsis_script  Name of resulting package.
 *
 * @return 0 success, -1 error.
 */
static int
execute_makensis (const gchar *nsis_script)
{
  gchar *dirname = g_path_get_dirname (nsis_script);
  gchar **cmd;
  gint exit_status;
  int ret = 0;
  gchar *standard_out = NULL;
  gchar *standard_err = NULL;

  cmd = (gchar **) g_malloc (3 * sizeof (gchar *));

  cmd[0] = g_strdup ("makensis");
  cmd[1] = g_strdup (nsis_script);
  cmd[2] = NULL;
  g_debug ("--- executing makensis.\n");
  g_debug ("%s: Spawning in %s: %s %s\n",
           __FUNCTION__,
           dirname, cmd[0], cmd[1]);
  if ((g_spawn_sync (dirname,
                     cmd,
                     NULL,                 /* Environment. */
                     G_SPAWN_SEARCH_PATH,
                     NULL,                 /* Setup func. */
                     NULL,
                     &standard_out,
                     &standard_err,
                     &exit_status,
                     NULL) == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      g_debug ("%s: failed to create the exe: %d (WIF %i, WEX %i)",
               __FUNCTION__,
               exit_status,
               WIFEXITED (exit_status),
               WEXITSTATUS (exit_status));
      g_debug ("%s: stdout: %s\n", __FUNCTION__, standard_out);
      g_debug ("%s: stderr: %s\n", __FUNCTION__, standard_err);
      ret = -1;
    }

  g_free (cmd[0]);
  g_free (cmd[1]);
  g_free (cmd);
  g_free (dirname);
  g_free (standard_out);
  g_free (standard_err);

  return ret;
}

/**
 * @brief Create an NSIS package.
 *
 * @param[in]  user_name    Name of user.
 * @param[in]  password     Password of user.
 * @param[in]  to_filename  Destination filename for package.
 *
 * @return 0 success, -1 error.
 */
static int
lsc_user_exe_create (const gchar *user_name, const gchar *password,
                     const gchar *to_filename)
{
  gchar *dirname = g_path_get_dirname (to_filename);
  gchar *nsis_script = g_build_filename (dirname, "p.nsis", NULL);

  g_free (dirname);

  if (create_nsis_script (nsis_script, to_filename, user_name, password))
    {
      g_warning ("%s: Failed to create NSIS script\n", __FUNCTION__);
      g_free (nsis_script);
      return -1;
    }

  if (execute_makensis (nsis_script))
    {
      g_warning ("%s: Failed to execute makensis\n", __FUNCTION__);
      g_free (nsis_script);
      return -1;
    }

  g_free (nsis_script);
  return 0;
}

/**
 * @brief Recreate NSIS package.
 *
 * @param[in]   name         User name.
 * @param[in]   password     Password.
 * @param[out]  exe          NSIS package.
 * @param[out]  exe_size     Size of NSIS package, in bytes.
 *
 * @return 0 success, -1 error.
 */
int
lsc_user_exe_recreate (const gchar *name, const gchar *password,
                       void **exe, gsize *exe_size)
{
  GError *error;
  char exe_dir[] = "/tmp/exe_XXXXXX";
  gchar *exe_path;
  int ret = -1;

  /* Create NSIS package. */

  if (mkdtemp (exe_dir) == NULL)
    return -1;
  exe_path = g_build_filename (exe_dir, "p.nsis", NULL);
  if (lsc_user_exe_create (name, password, exe_path))
    goto rm_exit;

  /* Read the package into memory. */

  error = NULL;
  g_file_get_contents (exe_path, (gchar **) exe, exe_size, &error);
  if (error)
    {
      g_error_free (error);
      goto rm_exit;
    }

  /* Return. */

  ret = 0;

 rm_exit:

  gvm_file_remove_recurse (exe_dir);

  g_free (exe_path);

  return ret;
}
