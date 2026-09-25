# INKademic 1.6.0 — versão oficial

Esta versão amplia a página de gerenciamento do dispositivo com um fluxo de atualização de firmware preparado para X4 Pro, X3/X4 e Sticky.

Ela também corrige a numeração usada durante o desenvolvimento do fork. As
antigas versões `1.6.1`, `1.7.x` e `1.8.0-rc` passam a integrar a linha
histórica `1.5.x`. O firmware reconhece esses identificadores antigos durante
a comparação de atualização, permitindo migrar diretamente para `1.6.0`.

## O que foi incorporado

- upload retomável em blocos de 64 KiB, preservando o parcial após uma queda de conexão;
- assinatura Ed25519 obrigatória, usando a chave pública já embutida no firmware;
- validação do formato ESP, chip, tamanho, identidade do modelo e ordem da versão antes do flash;
- segunda validação completa imediatamente antes da gravação na partição OTA;
- bloqueio preventivo quando a heap livre ou o maior bloco disponível estão baixos;
- cópia de segurança do candidato anterior e diagnóstico persistente no cartão SD;
- catálogo oficial assinado do GitHub e download direto pelo próprio navegador do dispositivo;
- confirmação explícita antes de reiniciar e instalação A/B, mantendo a partição ativa intacta até a troca;
- integração com rollback do bootloader e os limites de watchdog usados no fluxo de gravação;
- identidade embutida `INKADEMIC_FW_ID` em cada binário, impedindo instalar imagem de outro modelo.

## Assinaturas

O arquivo `.sig` contém os 64 bytes brutos da assinatura Ed25519 sobre o SHA-256 do `.bin`. A chave privada não fica no repositório nem no dispositivo; ela deve permanecer no ambiente de release autorizado. Sem uma assinatura correspondente, o navegador recusa a imagem antes de alterar a partição OTA.

O download oficial só ficará disponível quando a release do GitHub publicar, para cada modelo, o `.bin` e o `.bin.sig` correspondentes e o manifesto incluir o SHA-256.

## Verificação local

Foram validados os ambientes `simulator`, `default`, `sticky`, `x4-pro` e `recovery-x4-pro`. O binário do X4 Pro permanece dentro da partição OTA, com margem de aproximadamente 2,1 MB.

## Correções após os candidatos

Uma auditoria da versão originalmente publicada como `v1.8.0-rc-2`, agora
retificada para `v1.5.5-rc.2`, encontrou a verificação Ed25519 desativada nas
compilações de hardware. A `v1.6.0` ativa e exige essa verificação em todos os
modelos e também confere a memória flash antes de selecionar a nova partição.

A instalação manual de outros projetos continua disponível por SD, navegador e
URL OTA. Esses arquivos manuais não exigem assinatura do INKademic.

Também foram corrigidos o watchdog durante a gravação, o reinício ao abrir a
página do aparelho, a luz de fundo do X4 Pro, a preservação da luz em reinícios
internos, a liberação do Wi-Fi, a leitura de pressionamentos curtos e o cache
das páginas web por ETag.
