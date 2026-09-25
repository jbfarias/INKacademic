# Revisão da avaliação externa da v1.8.0-rc-2

Conferência em 24/09/2026. Base publicada `v1.8.0-rc-2`; os arquivos analisados
não diferem de `origin/main` nas áreas da configuração, servidor e renderização.
Esta seção registra o diagnóstico inicial. As correções posteriores, incluindo
instalação manual de outros projetos por SD, navegador e OTA, estão em
[update-corrections-validation.md](update-corrections-validation.md).

## Resultado crítico confirmado

As definições `HAVE_ED25519`, `HAVE_ED25519_VERIFY`,
`HAVE_ED25519_KEY_IMPORT` e `INKADEMIC_REQUIRE_SIGNED_OTA=1` estão no ambiente
`x4-pro-simulator`, não nos ambientes de hardware. O patch de wolfSSL não as
acrescenta. Os condicionais do verificador são avaliados antes de incluir o
header Ed25519, portanto um include posterior não pode ativá-los retroativamente.

Baixados diretamente da release e verificados independentemente com Ed25519
sobre o digest SHA-256, conforme `scripts/sign_ota.py`:

| Binário | Bytes | Assinatura externa | Chave pública no binário |
| --- | ---: | --- | --- |
| X3/X4 | 6.234.576 | Válida | Ausente |
| X4 Pro | 6.138.304 | Válida | Ausente |
| Sticky | 6.030.144 | Válida | Ausente |

No build local X4 Pro, a chave também está ausente e o mapa não contém
`wc_ed25519_verify_msg` nem `wc_ed25519_import_public`. A presença da unidade
`ed25519.c.o` no log de compilação não demonstra que suas funções foram
habilitadas ou ligadas.

Consequências pelo código: navegador rejeita assinatura válida; menu OTA com
assinatura rejeita por suporte indisponível; sem assinatura o menu OTA pode
prosseguir, pois a política obrigatória está desativada. Não houve reprodução
desses três fluxos no aparelho nesta revisão. A instalação 1.8.0 bem-sucedida
relatada pelo usuário no X4 Pro comprova aquele cenário de instalação, não a
validação Ed25519. O usuário também confirmou que, após a 1.8.0, instalou outro firmware pelo SD.

## Demais itens

| Item | Avaliação |
| --- | --- |
| 2 — Rotação | Falta um mecanismo de transição por OTA. Duas chaves podem ser adequadas numa rotação planejada, mas não devem ser adotadas automaticamente se houver comprometimento. Não há evidência de comprometimento nesta análise. As notas públicas da v1.8.0-rc mencionam instalação manual inicial por SD/recovery para trocar a chave; isso não implementa transição automática. |
| 3 — Identidade | Busca por byte com deslocamento de string e ausência de validação de caracteres confirmados. Prefixo literal isolado não basta para produzir erro: o parser também precisa encontrar os delimitadores seguintes dentro da janela. Tempo em segundos e falha universal por ordem do linker não foram demonstrados. |
| 4 — TOCTOU | Confirmado: valida, fecha e reabre sem verificar o digest efetivamente gravado antes da seleção de boot. O cenário de USB simultâneo não foi reproduzido; não confundir janela estrutural com exploração demonstrada. |
| 5 — Versão RC | Confirmado: workflow passa `INKADEMIC_RELEASE_VERSION=1`, e X4 Pro/Sticky priorizam esse valor. A rc-2 publicada veio de outro fluxo. |
| 6 — Imagem invertida | Confirmada divergência de implementação: normal transforma cada pixel, invertida transforma apenas a origem e copia linhas. Manifestação visual depende da orientação e bitmap. |
| 7 — Upload rejeitado | Confirmado: rejeição inicial não impede tratamento de END/ABORTED, com alteração do estado global e possível remoção do temporário. Resposta também depende do estado global. |
| 8 — Iluminação | Confirmado: alteração de default, sem migração que diferencie antigo zero padrão de escolha explícita. Corrigir documentação ou projetar migração com identidade de versão. |
| 9 — Binários Git | Confirmados 21 arquivos rastreados em `.pio`. Remover do índice em mudança própria; não apagar referências do usuário incidentalmente. |
| 10 — Robustez | Acúmulo de dígitos em `int` pode exceder limites; sufixos de dispositivo são tratados como produção; servidor habilita CORS e não implementa proteção de origem/CSRF nas rotas de firmware. Exploração via navegador depende também das suas políticas de acesso à rede local. |

## Ordem recomendada

1. Habilitar a verificação e a política obrigatória em hardware, com erro de
   compilação se ausentes. Testar assinatura válida, inválida e ausente no
   caminho de produção; presença da chave no binário é só uma checagem auxiliar.
2. Corrigir leitura de identidade e garantir que a imagem gravada corresponde
   à autenticada antes de selecionar o próximo boot.
3. Corrigir isolamento de requisição no upload, versão RC e limites do parser.
4. Proteger ações de firmware contra requisições de origem não autorizada,
   preservando o acesso legítimo do aplicativo companheiro.
5. Ajustar imagem invertida e documentação. Definir transição de chave antes
   de qualquer nova rotação.

Não promover a rc-2 como atualização assinada funcional. No momento desse
diagnóstico, o único patch local era o do OTA por setores. O relatório de
validação ligado acima registra o trabalho posterior, sem alterar essa
constatação sobre os binários publicados.
