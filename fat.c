#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

PartitionTable pt[4];
Fat16BootSector bs;
FILE *in;
Fat16Entry *actual_dir = NULL;
int entries;
unsigned short *fat;
void get_filename(Fat16Entry entry, char *filename, bool is_subdirectory);
bool print_file(Fat16Entry entry, int *file_size);
int get_start_of_root_dir();
void move_to_cluster(int cluster);
void read_file(char *filename, bool save);
void init_fs();
void dir();
void chdir();
void print_tree();
void load_initial_dir();

void get_filename(Fat16Entry entry, char *filename, bool is_subdirectory)
{
  memcpy(filename, entry.filename, sizeof(char) * 8);
  int start_of_ext = 8;
  for (int i = 0; i < 8; i++)
  {
    if (entry.filename[7 - i] != ' ')
    {
      start_of_ext = 8 - i;
      break;
    }
  }
  if (!is_subdirectory)
  {
    filename[start_of_ext++] = '.';
  }
  if (!is_subdirectory)
  {
    memcpy(filename + start_of_ext, entry.ext, sizeof(char) * 3);
  }
  else
  {
    memset(filename + start_of_ext, 0, 12 - start_of_ext);
  }
}
bool print_file(Fat16Entry entry, int *file_size)
{
  bool is_read_only = (entry.attributes & 0x01) > 0;
  bool hidden = (entry.attributes & 0x02) > 0;
  bool system = (entry.attributes & 0x04) > 0;
  bool subdirectory = (entry.attributes & 0x10) > 0;
  bool modified = (entry.attributes & 0x20) > 0;

  char hours = (entry.modify_time & (0b1111 << 11)) >> 11;
  char minutes = (entry.modify_time & (0b11111 << 5)) >> 5;
  char seconds = entry.modify_time & (0b1111);

  int year = ((entry.modify_date >> 9) & 127) + 1980;
  int month = (entry.modify_date & (0b1111 << 5)) >> 5;
  int day = (entry.modify_date & (0b11111));

  // Skip if filename was never used, see http://www.tavi.co.uk/phobos/fat.html#file_attributes
  if (entry.filename[0] != 0x00)
  {
    unsigned char filename[13] = {};
    get_filename(entry, filename, subdirectory);
    *file_size += entry.file_size;
    printf("%.2d.%.2d.%.4d %.2d:%.2d.%.2d %-12s %-5s %c%c%c%c start %8d size %8d B\n",
           day, month, year, hours, minutes, seconds, filename, subdirectory ? "<DIR>" : "",
           hidden ? 'H' : '-', system ? 'S' : '-', is_read_only ? 'R' : '-', modified ? 'M' : '-', entry.starting_cluster, entry.file_size);
  }

  if (subdirectory)
  {
    return true;
  }
  else
  {
    return false;
  }
}

int get_start_of_root_dir()
{
  int boot_sector = 512 * pt[0].start_sector + sizeof(Fat16BootSector);
  int root_directory_pos = (bs.reserved_sectors - 1 + bs.fat_size_sectors * bs.number_of_fats) * bs.sector_size;
  return boot_sector + root_directory_pos;
}
bool get_entry_by_name(char *filename, Fat16Entry *entry)
{
  for (int i = 0; i < entries; i++)
  {

    char entry_filename[13];
    memset(entry_filename, 0, sizeof(entry_filename));
    get_filename(actual_dir[i], entry_filename, (actual_dir[i].attributes & 0x10) > 0);
    for (int i = 0; i < strlen(entry_filename); i++)
    {
      //printf("|%c-%c|", filename[i], entry_filename[i]);
    }
    printf("\n");
    if (strcmp(filename, entry_filename) == 0)
    {
      *entry = (actual_dir[i]);
      return true;
    }
  }
  return false;
}
void move_to_cluster(int cluster)
{
  int start_of_root_dir = get_start_of_root_dir();
  int blocks_of_root_dir = ((bs.root_dir_entries * 32 + bs.sector_size - 1) / bs.sector_size) * bs.sector_size;
  int end_of_root_dir = start_of_root_dir + blocks_of_root_dir;
  int cluster_data = end_of_root_dir + ((cluster - 2) * bs.sectors_per_cluster) * bs.sector_size;
  fseek(in, cluster_data, SEEK_SET); //set from start
}
void read_file(char *filename, bool save)
{
  Fat16Entry entry;
  if (!get_entry_by_name(filename, &entry))
  {
    printf("file not found!\n");
    return;
  }

  unsigned short cluster = entry.starting_cluster;
  move_to_cluster(cluster);

  int size_of_cluster = bs.sectors_per_cluster * bs.sector_size;
  int file_left = entry.file_size;
  int cluster_left = size_of_cluster;

  unsigned char buffer[255];
  FILE *fp = NULL;
  if (save)
  {
    fp = fopen(filename, "w+");
  }
  while (file_left > 0 && cluster != 0xFFFF)
  {
    int bytes_to_read = sizeof(buffer);
    if (sizeof(buffer) > file_left)
      bytes_to_read = file_left;
    if (sizeof(buffer) > cluster_left)
      bytes_to_read = cluster_left;

    int bytes_read = fread(buffer, 1, bytes_to_read, in);
    if (!save)
    {
      for (int i = 0; i < bytes_read; i++)
      {
        printf("%c", buffer[i]);
      }
    }
    else
    {
      fwrite(buffer, bytes_read, sizeof(char), fp);
    }

    cluster_left -= bytes_read;
    file_left -= bytes_read;

    if (cluster_left == 0)
    {
      unsigned short next_cluster = fat[cluster];
      move_to_cluster(next_cluster);
      cluster = next_cluster;
      cluster_left = size_of_cluster;
    }
  }
  if (save)
  {
    printf("saved to %s \n", filename);
    fclose(fp);
  }
}
void chdir(char *directory)
{
  Fat16Entry entry;
  printf("changing to directory %s", directory);
  if (strcmp(directory, ".") == 0)
  {
    return;
  }
  memset(&entry, 0, sizeof(Fat16Entry));
  if (!get_entry_by_name(directory, &entry))
  {
    printf("directory not found!\n");
    return;
  }
  unsigned short cluster = entry.starting_cluster;
  if (cluster == 0x00)
  {
    load_initial_dir();
    return;
  }
  move_to_cluster(cluster);

  int size_of_cluster = bs.sectors_per_cluster * bs.sector_size;
  int cluster_left = size_of_cluster;

  if (actual_dir != NULL)
  {
    free(actual_dir);
    entries = 0;
  }
  actual_dir = malloc(bs.sectors_per_cluster * bs.sector_size);
  int clusters = 1;

  while (cluster != 0xFFFF)
  {
    Fat16Entry dir;
    memset(&dir, 0, sizeof(Fat16Entry));
    int bytes_to_read = sizeof(Fat16Entry);
    if (sizeof(Fat16Entry) > cluster_left)
      bytes_to_read = cluster_left;

    int bytes_read = fread(&dir, 1, bytes_to_read, in);
    if (dir.filename[0] == 0x00)
    {
      break;
    }
    if (dir.filename[0] != 0xe5)
    {
      //printf("%s \n", dir.filename);
      actual_dir[entries++] = dir;
    }

    cluster_left -= bytes_read;

    if (cluster_left == 0)
    {
      clusters++;
      actual_dir = realloc(actual_dir, clusters * (bs.sectors_per_cluster * bs.sector_size));
      unsigned short next_cluster = fat[cluster];
      move_to_cluster(next_cluster);
      cluster = next_cluster;
      cluster_left = size_of_cluster;
    }
  }
}
void load_initial_dir()
{
  int start_of_root_dir = get_start_of_root_dir();
  fseek(in, start_of_root_dir, SEEK_SET); //set from start
  if (actual_dir != NULL)
  {
    free(actual_dir);
    entries = 0;
  }
  actual_dir = malloc(bs.root_dir_entries * sizeof(Fat16Entry));
  for (int i = 0; i < bs.root_dir_entries; i++)
  {
    Fat16Entry entry;
    fread(&entry, sizeof(Fat16Entry), 1, in);
    if (entry.filename[0] != 0x00 && entry.filename[0] != 0xe5)
    {
      actual_dir[entries++] = entry;
    }
  }
}
void print_tree_recursive(char *name)
{
  chdir(name);
  dir();
  char entry_filename[255];
  bool is_subdirectory = false;
  for (int i = 0; i < entries; i++)
  {
    bool is_directory = (actual_dir[i].attributes & 0x10) > 0;
    if (is_directory)
    {
      get_filename(actual_dir[i], entry_filename, is_directory);
      if (strcmp(entry_filename, "..") == 0)
      {
        is_subdirectory = true;
      }
      if (strcmp(entry_filename, "..") == 0 || strcmp(entry_filename, ".") == 0)
      {
        continue;
      }
      print_tree_recursive(entry_filename);
    }
  }
  if (is_subdirectory)
  {
    chdir("..");
  }
}
void print_tree()
{
  load_initial_dir();
  print_tree_recursive(".");
}
void init_fs()
{
  in = fopen("sd.img", "rb");
  fseek(in, 0x1BE, SEEK_SET);               // go to partition table start, partitions start at offset 0x1BE, see http://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/HDPartitions.html
  fread(pt, sizeof(PartitionTable), 4, in); // read all entries (4)

  printf("Partition table\n-----------------------\n");
  for (int i = 0; i < 4; i++)
  { // for all partition entries print basic info
    printf("Partition %d, type %02X, ", i, pt[i].partition_type);
    printf("start sector %8d, length %8d sectors\n", pt[i].start_sector, pt[i].length_sectors);
  }

  printf("\nSeeking to first partition by %d sectors\n", pt[0].start_sector);
  fseek(in, 512 * pt[0].start_sector, SEEK_SET); // Boot sector starts here (seek in bytes)

  fread(&bs, sizeof(Fat16BootSector), 1, in); // Read boot sector content, see http://www.tavi.co.uk/phobos/fat.html#boot_block
  printf("Volume_label %.11s, %d sectors size\n", bs.volume_label, bs.sector_size);

  fseek(in, (bs.reserved_sectors - 1) * bs.sector_size, SEEK_CUR);
  fat = malloc(bs.fat_size_sectors * bs.sector_size * sizeof(char));
  fread(fat, bs.fat_size_sectors * bs.sector_size, 1, in);
  // Seek to the beginning of root directory, it's position is fixed
  fseek(in, (bs.fat_size_sectors * (bs.number_of_fats - 1)) * bs.sector_size, SEEK_CUR);
  printf("FS initialized\n");

  load_initial_dir();
}

void dir()
{
  // Read all entries of root directory
  printf("Filesystem directory listing\n-----------------------\n");
  int files = 0;
  int dirs = 0;
  int file_size = 0;
  for (int i = 0; i < entries; i++)
  {
    // Skip if filename was never used, see http://www.tavi.co.uk/phobos/fat.html#file_attributes
    if (actual_dir[i].filename[0] != 0x00)
    {
      if (print_file(actual_dir[i], &file_size))
      {
        dirs++;
      }
      else
      {
        files++;
      }
    }
  }
  printf("FILEs %d, Size %dB\nDIRs %d \n", files, file_size, dirs);
}

int main()
{
  printf("initialize fs:\n\n");
  init_fs();

  print_tree();
  /*
  printf("\n\nreading directory:\n\n");
  dir();

  printf("\n\nreading file ABSTRAKT.TXT:\n\n");
  read_file("ABSTRAKT.TXT", false);

  printf("\n\nreading file TEST.TXT:\n\n");
  read_file("TEST.TXT", false);

  printf("\n\nreading file FAT16.JPG:\n\n");
  read_file("FAT16.JPG", true);

  printf("\n\nchanging to directory ADR1:\n\n");
  chdir("ADR1");

  printf("\n\nreading directory:\n\n");
  dir();

  printf("\n\nreading file KOCKA.JPG:\n\n");
  read_file("KOCKA.JPG", true);

  printf("\n\nreading file VYUKA.JPG:\n\n");
  read_file("VYUKA.JPG", true);

  printf("\n\nchanging to directory .:\n\n");
  chdir(".");

  printf("\n\nchanging to directory parent:\n\n");
  chdir("..");
  dir();

  printf("\n\nchanging to directory parent:\n\n");
  chdir("ADR2");
  dir();

  printf("\n\nreading file HISTORIE.TXT:\n\n");
  read_file("HISTORIE.TXT", false);

  printf("\n\nreading file KOREN.TXT:\n\n");
  read_file("KOREN.TXT", false);*/

  char line[1024];
  while (true)
  {
    memset(line, 0, sizeof(line));
    printf("> ");
    fgets(line, sizeof(line), stdin);
    char *pos;
    if ((pos = strchr(line, '\n')) != NULL)
    {
      *pos = '\0';
    }
    char *cmd = NULL;
    char *arg = NULL;
    if (cmd = strtok(line, " "))
    {
      if (strcmp(cmd, "chdir") == 0)
      {
        arg = strtok(NULL, " ");
        chdir(arg);
      }
      else if (strcmp(cmd, "dir") == 0)
      {
        dir();
      }
      else if (strcmp(cmd, "exit") == 0)
      {
        return (0);
      }
      else if (strcmp(cmd, "read") == 0)
      {
        arg = strtok(NULL, " ");
        read_file(arg, false);
      }
      else if (strcmp(cmd, "save") == 0)
      {
        arg = strtok(NULL, " ");
        read_file(arg, true);
      }
    }
  }

  fclose(in);
  return 0;
}
