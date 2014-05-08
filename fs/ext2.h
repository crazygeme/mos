#ifndef _EXT2_H_
#define _EXT2_H_
struct _block;

void ext2_attach(struct _block* b);

/*
* Special inode numbers
*/
#define EXT2_BAD_INO         1  /* Bad blocks inode */
#define EXT2_ROOT_INO        2  /* Root inode */
#define EXT2_BOOT_LOADER_INO     5  /* Boot loader inode */
#define EXT2_UNDEL_DIR_INO   6  /* Undelete directory inode */

/* First non-reserved inode for old ext2 filesystems */
#define EXT2_GOOD_OLD_FIRST_INO 11  

#define EXT2_GOOD_OLD_INODE_SIZE 128

/*
* Constants relative to the data blocks
*/
#define	EXT2_NDIR_BLOCKS		12
#define	EXT2_IND_BLOCK			EXT2_NDIR_BLOCKS
#define	EXT2_DIND_BLOCK			(EXT2_IND_BLOCK + 1)
#define	EXT2_TIND_BLOCK			(EXT2_DIND_BLOCK + 1)
#define	EXT2_N_BLOCKS			(EXT2_TIND_BLOCK + 1)
/*
* Structure of the super block
*/
struct ext2_super_block {
	unsigned int	s_inodes_count;		/* Inodes count */
	unsigned int	s_blocks_count;		/* Blocks count */
	unsigned int	s_r_blocks_count;	/* Reserved blocks count */
	unsigned int	s_free_blocks_count;	/* Free blocks count */
	unsigned int	s_free_inodes_count;	/* Free inodes count */
	unsigned int	s_first_data_block;	/* First Data Block */
	unsigned int	s_log_block_size;	/* Block size */
	unsigned int	s_log_frag_size;	/* Fragment size */
	unsigned int	s_blocks_per_group;	/* # Blocks per group */
	unsigned int	s_frags_per_group;	/* # Fragments per group */
	unsigned int	s_inodes_per_group;	/* # Inodes per group */
	unsigned int	s_mtime;		/* Mount time */
	unsigned int	s_wtime;		/* Write time */
	unsigned short	s_mnt_count;		/* Mount count */
	unsigned short	s_max_mnt_count;	/* Maximal mount count */
	unsigned short	s_magic;		/* Magic signature */
	unsigned short	s_state;		/* File system state */
	unsigned short	s_errors;		/* Behaviour when detecting errors */
	unsigned short	s_minor_rev_level; 	/* minor revision level */
	unsigned int	s_lastcheck;		/* time of last check */
	unsigned int	s_checkinterval;	/* max. time between checks */
	unsigned int	s_creator_os;		/* OS */
	unsigned int	s_rev_level;		/* Revision level */
	unsigned short	s_def_resuid;		/* Default uid for reserved blocks */
	unsigned short	s_def_resgid;		/* Default gid for reserved blocks */
	/*
	* These fields are for EXT2_DYNAMIC_REV superblocks only.
	*
	* Note: the difference between the compatible feature set and
	* the incompatible feature set is that if there is a bit set
	* in the incompatible feature set that the kernel doesn't
	* know about, it should refuse to mount the filesystem.
	*
	* e2fsck's requirements are more strict; if it doesn't know
	* about a feature in either the compatible or incompatible
	* feature set, it must abort and not try to meddle with
	* things it doesn't understand...
	*/
	unsigned int	s_first_ino; 		/* First non-reserved inode */
	unsigned short   s_inode_size; 		/* size of inode structure */
	unsigned short	s_block_group_nr; 	/* block group # of this superblock */
	unsigned int	s_feature_compat; 	/* compatible feature set */
	unsigned int	s_feature_incompat; 	/* incompatible feature set */
	unsigned int	s_feature_ro_compat; 	/* readonly-compatible feature set */
	unsigned char	s_uuid[16];		/* 128-bit uuid for volume */
	char	s_volume_name[16]; 	/* volume name */
	char	s_last_mounted[64]; 	/* directory where last mounted */
	unsigned int	s_algorithm_usage_bitmap; /* For compression */
	/*
	* Performance hints.  Directory preallocation should only
	* happen if the EXT2_COMPAT_PREALLOC flag is on.
	*/
	unsigned char	s_prealloc_blocks;	/* Nr of blocks to try to preallocate*/
	unsigned char	s_prealloc_dir_blocks;	/* Nr to preallocate for dirs */
	unsigned short	s_padding1;
	/*
	* Journaling support valid if EXT3_FEATURE_COMPAT_HAS_JOURNAL set.
	*/
	unsigned char	s_journal_uuid[16];	/* uuid of journal superblock */
	unsigned int	s_journal_inum;		/* inode number of journal file */
	unsigned int	s_journal_dev;		/* device number of journal file */
	unsigned int	s_last_orphan;		/* start of list of inodes to delete */
	unsigned int	s_hash_seed[4];		/* HTREE hash seed */
	unsigned char	s_def_hash_version;	/* Default hash version to use */
	unsigned char	s_reserved_char_pad;
	unsigned short	s_reserved_word_pad;
	unsigned int	s_default_mount_opts;
	unsigned int	s_first_meta_bg; 	/* First metablock block group */
	unsigned int	s_reserved[190];	/* Padding to the end of the block */
};

struct ext2_bg_descriptor
{
	unsigned int bg_block_bitmap;// Block number of block bitmap
	unsigned int bg_inode_bitmap;// Block number of inode bitmap
	unsigned int bg_inode_table;// Block number of first inode table block
	unsigned short bg_free_blocks_count;// Number of free blocks in the group
	unsigned short bg_free_inodes_count;// Number of free inodes in the group
	unsigned short bg_used_dirs_count;// Number of directories in the group
	unsigned short bg_pad;// Alignment to word
	unsigned int  bg_reserved[3];// Nulls to pad out 24 bytes
};


/*
* Structure of an inode on the disk
*/
struct ext2_inode {
	unsigned short	i_mode;		/* File mode */
	unsigned short	i_uid;		/* Low 16 bits of Owner Uid */
	unsigned int	i_size;		/* Size in bytes */
	unsigned int	i_atime;	/* Access time */
	unsigned int	i_ctime;	/* Creation time */
	unsigned int	i_mtime;	/* Modification time */
	unsigned int	i_dtime;	/* Deletion Time */
	unsigned short	i_gid;		/* Low 16 bits of Group Id */
	unsigned short	i_links_count;	/* Links count */
	unsigned int	i_blocks;	/* Blocks count */
	unsigned int	i_flags;	/* File flags */
	union {
		struct {
			unsigned int  l_i_reserved1;
		} linux1;
		struct {
			unsigned int  h_i_translator;
		} hurd1;
		struct {
			unsigned int  m_i_reserved1;
		} masix1;
	} osd1;				/* OS dependent 1 */
	unsigned int	i_block[EXT2_N_BLOCKS];/* Pointers to blocks */
	unsigned int	i_generation;	/* File version (for NFS) */
	unsigned int	i_file_acl;	/* File ACL */
	unsigned int	i_dir_acl;	/* Directory ACL */
	unsigned int	i_faddr;	/* Fragment address */
	union {
		struct {
			unsigned char	l_i_frag;	/* Fragment number */
			unsigned char	l_i_fsize;	/* Fragment size */
			unsigned short	i_pad1;
			unsigned short	l_i_uid_high;	/* these 2 fields    */
			unsigned short	l_i_gid_high;	/* were reserved2[0] */
			unsigned int	l_i_reserved2;
		} linux2;
		struct {
			unsigned char	h_i_frag;	/* Fragment number */
			unsigned char	h_i_fsize;	/* Fragment size */
			unsigned short	h_i_mode_high;
			unsigned short	h_i_uid_high;
			unsigned short	h_i_gid_high;
			unsigned int	h_i_author;
		} hurd2;
		struct {
			unsigned char	m_i_frag;	/* Fragment number */
			unsigned char	m_i_fsize;	/* Fragment size */
			unsigned short	m_pad1;
			unsigned int	m_i_reserved2[2];
		} masix2;
	} osd2;				/* OS dependent 2 */
};


#define EXT2_NAME_LEN 255
/*
* The new version of the directory entry.  Since EXT2 structures are
* stored in intel byte order, and the name_len field could never be
* bigger than 255 chars, it's safe to reclaim the extra byte for the
* file_type field.
*/
struct ext2_dir_entry_2 {
	unsigned int	inode;			/* Inode number */
	unsigned short	rec_len;		/* Directory entry length */
	unsigned char	name_len;		/* Name length */
	unsigned char	file_type;
	char	name[EXT2_NAME_LEN];	/* File name */
};

struct filesys_type;
struct ext2_inode_info{
    struct filesys_type* type;
    struct ext2_inode* inode;
    unsigned inode_no;
    char name[EXT2_NAME_LEN];
};

struct ext2_dir_info{
    struct filesys_type* type;
    struct ext2_inode* inode;
    struct ext2_dir_entry_2* dir;
    char name[EXT2_NAME_LEN];
    unsigned cur;
    unsigned max;
    unsigned cur_block_idx;
    void* cur_block;

};
#endif
