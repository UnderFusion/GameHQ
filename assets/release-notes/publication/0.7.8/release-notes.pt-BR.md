# GameHQ 0.7.8 (2026-09-22)

## Destaques

- A sobreposição agora funciona de forma muito mais confiável sobre jogos em modo sem bordas. Ela aparece acima do jogo, captura a entrada do controle para a navegação na sobreposição em caminhos GameInput compatíveis, e o jogo continua visível. Confirmado com um DualSense com fio em Indiana Jones and the Great Circle no modo sem bordas (Borderless).
- Navegar pela sobreposição não controla mais o jogo por baixo dela em caminhos GameInput compatíveis. Ao fechar a sobreposição, o GameHQ espera você soltar os botões pressionados antes de devolver o controle ao jogo.
- Os clipes de replay estão mais seguros: salvamentos rápidos e repetidos nunca sobrescrevem um clipe existente, e é possível salvar um clipe logo após o início da gravação.
- Novas predefinições de mapeamento: crie, edite e atribua layouts de controle para cada controle e cada jogo.
- O GameHQ lembra onde você parou: a última página, a categoria de Configurações, o filtro da galeria e a categoria da sobreposição de cada jogo.

## Sobreposição

- A sobreposição pode assumir o foco do controle enquanto permanece acima de um jogo visível em modo sem bordas, sem minimizá-lo.
- Em caminhos GameInput compatíveis, navegar pela sobreposição não controla mais o jogo ao mesmo tempo.
- Ao ser fechada, a sobreposição aguarda um instante até que os botões pressionados e os analógicos voltem ao repouso antes de devolver o controle ao jogo, para que uma entrada mantida não passe para o jogo.
- Abrir e fechar a sobreposição ficou mais rápido e confiável, e um toque rápido repetido não a fecha mais logo depois de abrir. Alt-Tab e outras trocas intencionais para outro aplicativo são respeitadas.
- A sobreposição acompanha o jogo certo quando ele recria a janela, perde a janela ou perde o foco por um instante. Ela fecha corretamente quando outro aplicativo assume o primeiro plano.
- A sobreposição reabre na captura de tela ou no clipe que você selecionou por último. Depois de uma nova captura, ela começa no item mais recente.
- Os clipes começam a ser reproduzidos na sobreposição sem que a prévia desapareça por um instante.
- O fundo escurecido agora aparece junto com os menus da sobreposição, em vez de surgir gradualmente depois deles.
- As dicas de controles na parte inferior agora ficam sobre um pequeno fundo que segue sua configuração de escurecimento da sobreposição, para continuarem legíveis sobre o jogo.
- O jogo que você está jogando agora é destacado corretamente ao percorrer a barra lateral.
- A navegação com o controle funciona melhor nos menus da sobreposição, na galeria e na reprodução de vídeo, e os botões de navegação ficam separados dos atalhos de captura.

## Captura e replay

- Salvamentos rápidos e repetidos recebem nomes de arquivo exclusivos em vez de sobrescrever um clipe existente. Uma exportação com falha nunca remove clipes anteriores, e as miniaturas sempre correspondem ao clipe certo.
- Salvar um replay usa o que foi gravado até o momento, mesmo antes de a duração do buffer que você definiu ser preenchida.
- Uma exportação de replay mantém as imagens de que precisa quando você troca de jogo ou o buffer reinicia, e o GameHQ espera uma exportação em andamento terminar antes de encerrar.
- Capturas de tela e salvamentos de replay são confirmados imediatamente e depois mostram um resultado claro de sucesso ou falha. As notificações são atualizadas no mesmo lugar, e só um número limitado aparece ao mesmo tempo.
- Capturas que falham ou são ignoradas agora explicam o motivo, mesmo com as notificações de sucesso desativadas.
- O status do replay agora mostra se a gravação realmente começou e se já há imagens utilizáveis, com mensagens claras para os estados de inicialização, buffer vazio e exportação em andamento.
- As sessões manuais de replay agora resistem a mudanças nas configurações e a capturas de tela HDR feitas ao mesmo tempo. Uma sessão manual iniciada, mas nunca usada, é desligada depois de um tempo.
- Salvar um replay não trava mais a captura enquanto a miniatura é criada, e capturas de tela feitas ao mesmo tempo criam sua pasta de forma mais confiável.
- Nova opção para a borda amarela de captura do Windows, com informações mais claras sobre permissão e suporte do sistema. A gravação continua funcionando quando o Windows não consegue ocultar a borda.

## Controles e entrada

- Detecção de controles e encaminhamento de entrada mais confiáveis quando vários controles ou fontes de entrada estão conectados.
- Trocar de fonte de entrada, reconectar controles e acompanhar botões mantidos pressionados agora evita toques perdidos, toques duplicados e entrada travada.
- Corrigidos toques no botão PS que não eram detectados e toques repetidos atrasados que podiam reabrir a sobreposição logo depois de você fechá-la.
- Gatilhos, cliques dos analógicos e outros botões agora se comportam da mesma forma em todas as fontes de entrada compatíveis, então as atribuições funcionam de modo mais previsível.
- Controles que usam GameInput continuam funcionando normalmente, incluindo o isolamento da sobreposição, quando o Windows não consegue oferecer o suporte opcional aos botões Guide/Share.
- O controle que você está editando em Configurações continua selecionado quando outro controle se torna ativo.

## Predefinições de mapeamento

- Nova biblioteca de predefinições para layouts de controle, onde você pode criar, renomear, duplicar, editar e excluir predefinições.
- As predefinições podem ser atribuídas a controles e jogos, com uma opção de reserva e seleção automática para o jogo em execução.
- Suas atribuições personalizadas existentes passam automaticamente para o sistema de predefinições, e os dados originais são mantidos como backup.
- A troca de predefinição leva em conta botões mantidos pressionados e gestos em andamento, para que a troca não acione ações por acidente.
- A interface separa a predefinição atribuída da que você está editando, protege edições não salvas e permite criar uma cópia para um único controle.
- Excluir uma predefinição em uso pede primeiro uma substituta ou uma opção de reserva, e alterações em predefinições compartilhadas são claramente sinalizadas.

## Interface, configurações e som

- O GameHQ reabre na última página, categoria de Configurações e filtro da galeria que você usou. A sobreposição lembra sua última categoria separadamente para cada jogo.
- A janela agora é restaurada corretamente em monitores à esquerda ou acima da tela principal. Uma janela que abriria totalmente fora da tela é movida de volta para uma tela conectada.
- A janela principal e a sobreposição têm, cada uma, sua própria escala, de 100% a 200%, mantida entre reinicializações. Janelas pequenas ganharam um layout melhor.
- Os sons de captura estão mais altos e distintos e têm controle de volume e prévia próprios. O volume da interface e o da captura podem chegar a 300%.
- Alterar as configurações de notificação, som, borda de captura ou sessão manual não descarta mais o seu buffer de replay.
- A galeria abre com o filtro salvo. Um filtro salvo para um jogo que não existe mais volta a um padrão seguro. As categorias de Configurações continuam no lugar mesmo se a ordem delas mudar.
- Corrigidos conteúdo em branco e problemas na ordem de camadas ao mudar a escala da interface, e os menus agora aparecem acima do conteúdo principal.
- Os controles de atribuição mostram quanto tempo dura um toque mantido e explicam como funcionam os gestos de manter pressionado.

## Atualizações, idiomas e diagnóstico

- As notas de atualização aparecem no idioma selecionado, usam o inglês quando não há tradução disponível e continuam legíveis offline depois de carregadas.
- As notas de atualização podem incluir um link opcional para a versão no GitHub, que você pode acessar com o controle.
- As traduções foram ampliadas em todos os idiomas compatíveis para as novas mensagens de captura, som, borda, manter pressionado, predefinições e foco.
- O diagnóstico copiado agora inclui as atribuições ativas, a seleção de predefinição, as mudanças de fonte de entrada e o estado de foco da sobreposição, com identificadores de dispositivo anonimizados.
- O diagnóstico de captura acompanha cada solicitação desde o toque no botão até o arquivo salvo, o que facilita encontrar problemas. Problemas de som são informados com um aviso claro.

## Limitações conhecidas

- O isolamento do controle em relação ao jogo depende do jogo e de como ele lê o controle. Um DualSense com fio usando GameInput foi bem testado. O isolamento não é garantido para XInput, Raw Input, HID direto, Steam Input ou controles virtuais.
- A troca de controles com o DSX em execução foi verificada apenas parcialmente. As configurações do DSX continuam sob seu controle; o GameHQ não instala nem gerencia drivers de controle virtual ou de ocultação de dispositivos.
- Alguns jogos pausam ou reagem quando perdem o foco. O Windows ou outro aplicativo de captura pode manter visível a borda amarela de gravação.
- Quando uma nota de versão 0.7.8 localizada não estiver disponível, o GameHQ exibe a versão em inglês.
