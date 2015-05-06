#ifndef __COMPAT_DENTRY_H__
#   define __COMPAT_DENTRY_H__

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
# define DENTRY(file) (file->f_path.dentry)
#else
# define DENTRY(file) (file->f_dentry)
#endif

#endif /* __COMPAT_DENTRY_H__ */
