    .global blob_tar
    .global size_tar
    .section .rodata
blob_tar:
    .incbin "blob.tar.gz"
1:
size_tar:
    .int 1b - blob_tar
