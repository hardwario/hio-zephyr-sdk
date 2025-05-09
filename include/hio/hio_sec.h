#ifndef HIO_SEC_H_
#define HIO_SEC_H_

int hio_root_cert_write(int security_tag_index, const char *cert, int cert_len);
int hio_root_cert_delete(int security_tag_index);

int hio_cert_write(int security_tag_index, const char *cert, int cert_len);
int hio_cert_delete(int security_tag_index);

int hio_prv_key_generate(int security_tag_index);
int hio_prv_key_write(int security_tag_index, const char *key, int key_len);
int hio_prv_key_delete(int security_tag_index);

#endif /* HIO_SEC_H_ */
