# RCOM Lab2 

**Elementos:**
- Helena Rocha
- Ana Sofia Ferreira

## Estrutura

- `examples/` - Código exemplo fornecido nas aulas (`getip.c`, `clientTCP.c`)
- `src/` - Implementação do cliente FTP (`ftp_download.c`)

## Compilar

    make

## Executar

    ./download ftp://[user:pass@]host/path

## Exemplos

    ./download ftp://demo:password@test.rebex.net/readme.txt

Na aula (rede I322):

    ./download ftp://ftp.netlab.fe.up.pt/pub/teste.txt
    ./download ftp://rcom:rcom@ftp.netlab.fe.up.pt/var/ftp/pub/teste.txt