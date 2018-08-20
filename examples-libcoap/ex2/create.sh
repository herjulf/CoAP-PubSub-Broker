# create topic and subtopic. Note content-type
coap-client -m post   coap://127.0.0.1:/ps  -e "<topic>;ct=40"
coap-client -m post   coap://127.0.0.1:/ps/topic  -e "<temp>;ct=0"
